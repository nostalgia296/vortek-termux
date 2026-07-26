#include <errno.h>
#include <stdarg.h>

#include "wrapper_compat.h"
#include "vk_context.h"
#include "vulkan_helper.h"

#define VORTEK_MAX_PROFILE_EXTENSIONS 8
#define VORTEK_PUSH_POOL_CHUNK 64
#define VORTEK_MAX_PUSH_POOL_SIZES 16
#define VORTEK_MAX_PUSH_DESCRIPTORS 32
#define VORTEK_D3D_BINDLESS_LIMIT (1u << 20)

typedef struct PushDescriptorLayout {
    VkDescriptorSetLayout layout;
    VkDescriptorPoolCreateFlags poolFlags;
    VkDescriptorPoolSize sizes[VORTEK_MAX_PUSH_POOL_SIZES];
    uint32_t sizeCount;
} PushDescriptorLayout;

typedef struct PushPipelineLayout {
    VkPipelineLayout layout;
    uint32_t setLayoutCount;
    VkDescriptorSetLayout* setLayouts;
} PushPipelineLayout;

typedef struct PushDescriptorPool {
    VkDescriptorPool pool;
    VkDescriptorSetLayout layout;
    uint32_t remaining;
    struct PushDescriptorPool* next;
} PushDescriptorPool;

typedef struct PushCommandBuffer {
    VkCommandBuffer commandBuffer;
    VkCommandPool commandPool;
    PushDescriptorPool* pools;
} PushCommandBuffer;

struct VortekWrapperCompat {
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkDriverId driverId;
    uint32_t apiVersion;
    uint32_t driverVersion;
    uint32_t engineVersion;
    char applicationName[VK_MAX_DESCRIPTION_SIZE];
    char engineName[VK_MAX_DESCRIPTION_SIZE];

    ArrayList nativeExtensions;
    ArrayList pushLayouts;
    ArrayList pipelineLayouts;
    ArrayList commandBuffers;

    const char* disabledExtensions[VORTEK_MAX_PROFILE_EXTENSIONS];
    uint32_t disabledExtensionCount;
    const char* emulatedExtensions[VORTEK_MAX_PROFILE_EXTENSIONS];
    uint32_t emulatedExtensionCount;

    bool isD3D;
    bool isDxvk;
    bool isVkd3d;
    bool safeCreate;
    bool diagnostics;
    bool disablePresentWait;
    bool emulatePushDescriptors;
    bool emulateNullDescriptor;
    bool deviceFaultRequested;
    bool deviceFaultEnabled;
    bool baseNullDescriptor;
    bool baseDeviceFault;

    VkPhysicalDeviceFaultFeaturesEXT faultFeatures;
    PFN_vkGetDeviceFaultInfoEXT getDeviceFaultInfo;

    VkBuffer nullBuffer;
    VkDeviceMemory nullBufferMemory;
    VkImage nullImage;
    VkDeviceMemory nullImageMemory;
    VkImageView nullImageView;
    VkSampler nullSampler;
};

static const char* compatEnv(const char* vortekName, const char* wrapperName) {
    const char* value = getenv(vortekName);
    if ((!value || !value[0]) && wrapperName) value = getenv(wrapperName);
    return value && value[0] ? value : NULL;
}

static bool compatEnvBool(const char* vortekName, const char* wrapperName,
                          bool defaultValue) {
    const char* value = compatEnv(vortekName, wrapperName);
    return value ? atoi(value) != 0 : defaultValue;
}

static uint32_t compatEnvUint(const char* vortekName, const char* wrapperName) {
    const char* value = compatEnv(vortekName, wrapperName);
    if (!value) return 0;
    char* end = NULL;
    unsigned long parsed = strtoul(value, &end, 0);
    return end && *end == '\0' && parsed <= UINT32_MAX ? (uint32_t)parsed : 0;
}

static bool hasName(const char* haystack, const char* needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

static bool nativeExtensionSupported(const VortekWrapperCompat* compat,
                                     const char* extensionName) {
    if (!compat || !extensionName) return false;
    for (int i = 0; i < compat->nativeExtensions.size; i++) {
        VkExtensionProperties* property = compat->nativeExtensions.elements[i];
        if (strcmp(property->extensionName, extensionName) == 0) return true;
    }
    return false;
}

static bool extensionEnabled(const VkDeviceCreateInfo* createInfo,
                             const char* extensionName) {
    if (!createInfo || !extensionName) return false;
    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++) {
        if (strcmp(createInfo->ppEnabledExtensionNames[i], extensionName) == 0)
            return true;
    }
    return false;
}

static void addProfileExtension(const char** extensions, uint32_t* count,
                                const char* extensionName) {
    if (*count >= VORTEK_MAX_PROFILE_EXTENSIONS) return;
    for (uint32_t i = 0; i < *count; i++) {
        if (strcmp(extensions[i], extensionName) == 0) return;
    }
    extensions[(*count)++] = extensionName;
}

static void queryNativeExtensions(VortekWrapperCompat* compat) {
    uint32_t count = 0;
    if (vulkanWrapper.vkEnumerateDeviceExtensionProperties(
            compat->physicalDevice, NULL, &count, NULL) != VK_SUCCESS || count == 0)
        return;

    VkExtensionProperties* properties = calloc(count, sizeof(*properties));
    if (!properties) return;
    if (vulkanWrapper.vkEnumerateDeviceExtensionProperties(
            compat->physicalDevice, NULL, &count, properties) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            VkExtensionProperties* property = malloc(sizeof(*property));
            if (!property) continue;
            *property = properties[i];
            ArrayList_add(&compat->nativeExtensions, property);
        }
    }
    free(properties);
}

static void setupProfile(VortekWrapperCompat* compat) {
    bool isArm = compat->driverId == VK_DRIVER_ID_ARM_PROPRIETARY;
    bool isQualcomm = compat->driverId == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
    bool isSamsung = compat->driverId == VK_DRIVER_ID_SAMSUNG_PROPRIETARY;

    if (compat->disablePresentWait)
        addProfileExtension(compat->disabledExtensions,
                            &compat->disabledExtensionCount,
                            VK_KHR_PRESENT_WAIT_EXTENSION_NAME);

    if (isQualcomm) {
        addProfileExtension(compat->disabledExtensions,
                            &compat->disabledExtensionCount,
                            VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
        if (compat->isDxvk)
            addProfileExtension(compat->disabledExtensions,
                                &compat->disabledExtensionCount,
                                VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME);
    }

    if (isArm) {
        addProfileExtension(compat->disabledExtensions,
                            &compat->disabledExtensionCount,
                            VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
        if (!compat->isD3D) {
            addProfileExtension(compat->disabledExtensions,
                                &compat->disabledExtensionCount,
                                VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
            addProfileExtension(compat->disabledExtensions,
                                &compat->disabledExtensionCount,
                                VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
        }

        if (!nativeExtensionSupported(compat,
                                      VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME))
            addProfileExtension(compat->emulatedExtensions,
                                &compat->emulatedExtensionCount,
                                VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
        if (!nativeExtensionSupported(compat,
                                      VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME))
            addProfileExtension(compat->emulatedExtensions,
                                &compat->emulatedExtensionCount,
                                VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    }

    if (isArm && compat->isD3D &&
        !nativeExtensionSupported(compat, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME))
        addProfileExtension(compat->emulatedExtensions,
                            &compat->emulatedExtensionCount,
                            VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);

    if (isSamsung && compat->isD3D &&
        !nativeExtensionSupported(
            compat, VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME))
        addProfileExtension(
            compat->emulatedExtensions, &compat->emulatedExtensionCount,
            VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME);

    if (compat->isD3D) {
        /* Maintenance5 is handled by client-side aliases, even when the base
         * driver has it, because old Vortek protocol revisions cannot carry
         * its feature structure. */
        addProfileExtension(compat->emulatedExtensions,
                            &compat->emulatedExtensionCount,
                            VK_KHR_MAINTENANCE_5_EXTENSION_NAME);

        if (!nativeExtensionSupported(compat, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) ||
            compatEnvBool("VORTEK_EMULATE_PUSH_DESCRIPTOR",
                          "WRAPPER_EMULATE_PUSH_DESCRIPTOR", false))
            addProfileExtension(compat->emulatedExtensions,
                                &compat->emulatedExtensionCount,
                                VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);

        if ((isArm || isQualcomm) && compat->isDxvk &&
            compat->engineVersion >= VK_MAKE_VERSION(2, 7, 0) &&
            !nativeExtensionSupported(compat, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME))
            addProfileExtension(compat->emulatedExtensions,
                                &compat->emulatedExtensionCount,
                                VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    }
}

static bool isEmulatedExtension(const VortekWrapperCompat* compat,
                                const char* extensionName) {
    for (uint32_t i = 0; compat && i < compat->emulatedExtensionCount; i++) {
        if (strcmp(compat->emulatedExtensions[i], extensionName) == 0) return true;
    }
    return false;
}

static void aliasVertexDivisorExtension(VkContext* context,
                                        VkDeviceCreateInfo* createInfo) {
    VortekWrapperCompat* compat = context->wrapperCompat;
    bool extEnabled = extensionEnabled(
        createInfo, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    bool khrEnabled = extensionEnabled(
        createInfo, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    const char* alias = NULL;

    /* Both aliases are advertised on Mali. Forward whichever spelling the
     * base driver really implements so r51's native KHR path is not lost when
     * an older D3D stack asks for EXT (and keep the reverse case symmetric). */
    if (extEnabled && !khrEnabled &&
        !nativeExtensionSupported(
            compat, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) &&
        nativeExtensionSupported(
            compat, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME))
        alias = VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    else if (khrEnabled && !extEnabled &&
             !nativeExtensionSupported(
                 compat, VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) &&
             nativeExtensionSupported(
                 compat, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME))
        alias = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;

    if (!alias) return;
    const char** names = vt_alloc(
        &context->memoryPool,
        (createInfo->enabledExtensionCount + 1) * sizeof(*names));
    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++)
        names[i] = createInfo->ppEnabledExtensionNames[i];
    names[createInfo->enabledExtensionCount++] = alias;
    createInfo->ppEnabledExtensionNames = names;
}

static void removeEmulatedExtensions(VkContext* context,
                                     VkDeviceCreateInfo* createInfo) {
    VortekWrapperCompat* compat = context->wrapperCompat;
    if (!compat || createInfo->enabledExtensionCount == 0) return;

    const char** names = vt_alloc(&context->memoryPool,
                                  createInfo->enabledExtensionCount * sizeof(*names));
    uint32_t count = 0;
    for (uint32_t i = 0; i < createInfo->enabledExtensionCount; i++) {
        const char* name = createInfo->ppEnabledExtensionNames[i];
        bool filtered = isEmulatedExtension(compat, name);
        for (int j = 0; !filtered && context->disabledDeviceExtensions &&
                        j < context->disabledDeviceExtensions->size; j++) {
            filtered = strcmp(name,
                              context->disabledDeviceExtensions->elements[j]) == 0;
        }
        if (!filtered) names[count++] = name;
    }
    createInfo->ppEnabledExtensionNames = names;
    createInfo->enabledExtensionCount = count;
}

static void unlinkStructure(VkDeviceCreateInfo* createInfo,
                            VkBaseOutStructure* target) {
    VkBaseOutStructure* previous = NULL;
    VkBaseOutStructure* current = (VkBaseOutStructure*)createInfo->pNext;
    while (current) {
        if (current == target) {
            if (previous) previous->pNext = current->pNext;
            else createInfo->pNext = current->pNext;
            return;
        }
        previous = current;
        current = current->pNext;
    }
}

static void filterEmulatedFeatureStructures(VortekWrapperCompat* compat,
                                            VkDeviceCreateInfo* createInfo) {
    VkBaseOutStructure* current = (VkBaseOutStructure*)createInfo->pNext;
    while (current) {
        VkBaseOutStructure* next = current->pNext;
        bool remove = false;
        switch (current->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT: {
                VkPhysicalDeviceRobustness2FeaturesEXT* features = (void*)current;
                if (isEmulatedExtension(compat, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) {
                    compat->emulateNullDescriptor = features->nullDescriptor;
                    remove = true;
                }
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR:
                remove = !nativeExtensionSupported(
                             compat,
                             VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME) &&
                         !nativeExtensionSupported(
                             compat,
                             VK_KHR_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
                remove = compat->apiVersion < VK_API_VERSION_1_1;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
                remove = compat->apiVersion < VK_API_VERSION_1_2;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
                remove = compat->apiVersion < VK_API_VERSION_1_3;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR:
                remove = true;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT:
                remove = isEmulatedExtension(
                    compat,
                    VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME);
                break;
            default:
                break;
        }
        if (remove) unlinkStructure(createInfo, current);
        current = next;
    }
}

static void addDeviceFaultFeature(VkContext* context,
                                  VkDeviceCreateInfo* createInfo) {
    VortekWrapperCompat* compat = context->wrapperCompat;
    if (!compat || !compat->deviceFaultRequested ||
        !nativeExtensionSupported(compat, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) ||
        !compat->baseDeviceFault)
        return;

    const char* extension = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    injectExtensions(context, (char***)&createInfo->ppEnabledExtensionNames,
                     &createInfo->enabledExtensionCount, &extension, 1, NULL, 0);

    compat->faultFeatures = (VkPhysicalDeviceFaultFeaturesEXT){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
        .pNext = (void*)createInfo->pNext,
        .deviceFault = VK_TRUE,
        .deviceFaultVendorBinary = VK_FALSE,
    };
    createInfo->pNext = &compat->faultFeatures;
}

void VortekWrapperCompat_init(VkContext* context, VkPhysicalDevice physicalDevice,
                              const VkApplicationInfo* applicationInfo) {
    VortekWrapperCompat_destroy(context);
    VortekWrapperCompat* compat = calloc(1, sizeof(*compat));
    if (!compat) return;
    context->wrapperCompat = compat;
    compat->physicalDevice = physicalDevice;
    compat->safeCreate = compatEnvBool("VORTEK_SAFE_CREATE_DEVICE",
                                       "WRAPPER_SAFE_CREATE_DEVICE", true);
    compat->diagnostics = compatEnvBool("VORTEK_DIAG", "WRAPPER_DIAG", false);
    compat->disablePresentWait = compatEnvBool(
        "VORTEK_DISABLE_PRESENT_WAIT", "WRAPPER_DISABLE_PRESENT_WAIT", false);
    compat->deviceFaultRequested = compatEnvBool("VORTEK_DEVICE_FAULT",
                                                 "WRAPPER_DEVICE_FAULT", true);

    if (applicationInfo) {
        compat->engineVersion = applicationInfo->engineVersion;
        if (applicationInfo->pApplicationName)
            snprintf(compat->applicationName, sizeof(compat->applicationName), "%s",
                     applicationInfo->pApplicationName);
        if (applicationInfo->pEngineName)
            snprintf(compat->engineName, sizeof(compat->engineName), "%s",
                     applicationInfo->pEngineName);
    }
    compat->isDxvk = hasName(compat->engineName, "DXVK");
    compat->isVkd3d = hasName(compat->engineName, "vkd3d");
    compat->isD3D = compat->isDxvk || compat->isVkd3d;

    VkPhysicalDeviceDriverProperties driver = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &driver,
    };
    vulkanWrapper.vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
    compat->driverId = driver.driverID;
    compat->apiVersion = properties.properties.apiVersion;
    compat->driverVersion = properties.properties.driverVersion;

    queryNativeExtensions(compat);

    VkPhysicalDeviceRobustness2FeaturesEXT robustness2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
    };
    VkPhysicalDeviceFaultFeaturesEXT fault = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,
    };
    VkPhysicalDeviceFeatures2 features = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
    };
    if (nativeExtensionSupported(compat, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) {
        robustness2.pNext = features.pNext;
        features.pNext = &robustness2;
    }
    if (nativeExtensionSupported(compat, VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) {
        fault.pNext = features.pNext;
        features.pNext = &fault;
    }
    if (features.pNext)
        vulkanWrapper.vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
    compat->baseNullDescriptor = robustness2.nullDescriptor;
    compat->baseDeviceFault = fault.deviceFault;

    setupProfile(compat);
}

static void freePushPools(VkDevice device, PushDescriptorPool* pool) {
    while (pool) {
        PushDescriptorPool* next = pool->next;
        if (device && pool->pool)
            vulkanWrapper.vkDestroyDescriptorPool(device, pool->pool, NULL);
        free(pool);
        pool = next;
    }
}

static void clearTracking(VortekWrapperCompat* compat, VkDevice device) {
    for (int i = compat->commandBuffers.size - 1; i >= 0; i--) {
        PushCommandBuffer* command = compat->commandBuffers.elements[i];
        freePushPools(device, command->pools);
        free(command);
    }
    MEMFREE(compat->commandBuffers.elements);
    compat->commandBuffers = (ArrayList){0};

    for (int i = compat->pipelineLayouts.size - 1; i >= 0; i--) {
        PushPipelineLayout* layout = compat->pipelineLayouts.elements[i];
        MEMFREE(layout->setLayouts);
        free(layout);
    }
    MEMFREE(compat->pipelineLayouts.elements);
    compat->pipelineLayouts = (ArrayList){0};

    for (int i = compat->pushLayouts.size - 1; i >= 0; i--)
        free(compat->pushLayouts.elements[i]);
    MEMFREE(compat->pushLayouts.elements);
    compat->pushLayouts = (ArrayList){0};
}

void VortekWrapperCompat_destroy(VkContext* context) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (!compat) return;
    clearTracking(compat, VK_NULL_HANDLE);
    ArrayList_free(&compat->nativeExtensions, true);
    free(compat);
    context->wrapperCompat = NULL;
}

const char* const* VortekWrapperCompat_getDisabledExtensions(VkContext* context,
                                                             uint32_t* count) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    *count = compat ? compat->disabledExtensionCount : 0;
    return compat ? compat->disabledExtensions : NULL;
}

const char* const* VortekWrapperCompat_getEmulatedExtensions(VkContext* context,
                                                             uint32_t* count) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    *count = compat ? compat->emulatedExtensionCount : 0;
    return compat ? compat->emulatedExtensions : NULL;
}

void VortekWrapperCompat_applyFeatures(VkContext* context,
                                       VkPhysicalDeviceFeatures* features,
                                       void* pNext) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (!compat) return;
    bool isArm = compat->driverId == VK_DRIVER_ID_ARM_PROPRIETARY;
    bool isQualcomm = compat->driverId == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;

    if (isArm && compat->isD3D) {
        features->dualSrcBlend = VK_TRUE;
        features->multiDrawIndirect = VK_TRUE;
    }

    for (VkBaseOutStructure* item = pNext; item; item = item->pNext) {
        switch (item->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT:
                if (isArm && compat->isD3D) {
                    VkPhysicalDeviceRobustness2FeaturesEXT* robustness = (void*)item;
                    robustness->robustBufferAccess2 = VK_TRUE;
                    robustness->nullDescriptor = VK_TRUE;
                    if (compat->isVkd3d) robustness->robustImageAccess2 = VK_TRUE;
                }
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT:
                if (isArm && compat->isD3D)
                    ((VkPhysicalDeviceExtendedDynamicStateFeaturesEXT*)item)
                        ->extendedDynamicState = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT:
                if (isArm && compat->isD3D)
                    ((VkPhysicalDeviceExtendedDynamicState2FeaturesEXT*)item)
                        ->extendedDynamicState2 = VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GLOBAL_PRIORITY_QUERY_FEATURES_KHR:
                if (isQualcomm &&
                    compat->driverVersion > VK_MAKE_VERSION(512, 744, 0) &&
                    hasName(compat->applicationName, "clvk"))
                    ((VkPhysicalDeviceGlobalPriorityQueryFeaturesKHR*)item)
                        ->globalPriorityQuery = VK_FALSE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR:
                if (compat->disablePresentWait)
                    ((VkPhysicalDevicePresentWaitFeaturesKHR*)item)->presentWait =
                        VK_FALSE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR:
                if (isArm) {
                    VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR* divisor =
                        (void*)item;
                    divisor->vertexAttributeInstanceRateDivisor = VK_TRUE;
                    divisor->vertexAttributeInstanceRateZeroDivisor = VK_TRUE;
                }
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR:
                if (compat->isD3D)
                    ((VkPhysicalDeviceMaintenance5FeaturesKHR*)item)->maintenance5 =
                        VK_TRUE;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT:
                if (isEmulatedExtension(
                        compat,
                        VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME))
                    ((VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT*)item)
                        ->dynamicRenderingUnusedAttachments = VK_TRUE;
                break;
            default:
                break;
        }
    }
}

#define BUMP_UAB_LIMIT(property, target) do { \
    if ((property)->maxUpdateAfterBindDescriptorsInAllPools < (target)) \
        (property)->maxUpdateAfterBindDescriptorsInAllPools = (target); \
    if ((property)->maxPerStageDescriptorUpdateAfterBindSamplers < (target)) \
        (property)->maxPerStageDescriptorUpdateAfterBindSamplers = (target); \
    if ((property)->maxPerStageDescriptorUpdateAfterBindUniformBuffers < (target)) \
        (property)->maxPerStageDescriptorUpdateAfterBindUniformBuffers = (target); \
    if ((property)->maxPerStageDescriptorUpdateAfterBindStorageBuffers < (target)) \
        (property)->maxPerStageDescriptorUpdateAfterBindStorageBuffers = (target); \
    if ((property)->maxPerStageDescriptorUpdateAfterBindSampledImages < (target)) \
        (property)->maxPerStageDescriptorUpdateAfterBindSampledImages = (target); \
    if ((property)->maxPerStageDescriptorUpdateAfterBindStorageImages < (target)) \
        (property)->maxPerStageDescriptorUpdateAfterBindStorageImages = (target); \
    if ((property)->maxPerStageUpdateAfterBindResources < (target)) \
        (property)->maxPerStageUpdateAfterBindResources = (target); \
    if ((property)->maxDescriptorSetUpdateAfterBindSamplers < (target)) \
        (property)->maxDescriptorSetUpdateAfterBindSamplers = (target); \
    if ((property)->maxDescriptorSetUpdateAfterBindUniformBuffers < (target)) \
        (property)->maxDescriptorSetUpdateAfterBindUniformBuffers = (target); \
    if ((property)->maxDescriptorSetUpdateAfterBindStorageBuffers < (target)) \
        (property)->maxDescriptorSetUpdateAfterBindStorageBuffers = (target); \
    if ((property)->maxDescriptorSetUpdateAfterBindSampledImages < (target)) \
        (property)->maxDescriptorSetUpdateAfterBindSampledImages = (target); \
    if ((property)->maxDescriptorSetUpdateAfterBindStorageImages < (target)) \
        (property)->maxDescriptorSetUpdateAfterBindStorageImages = (target); \
} while (0)

void VortekWrapperCompat_applyProperties(VkContext* context,
                                         VkPhysicalDeviceProperties* properties,
                                         void* pNext) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (!compat) return;

    const char* deviceName = compatEnv("VORTEK_DEVICE_NAME", "WRAPPER_DEVICE_NAME");
    uint32_t deviceId = compatEnvUint("VORTEK_DEVICE_ID", "WRAPPER_DEVICE_ID");
    uint32_t vendorId = compatEnvUint("VORTEK_VENDOR_ID", "WRAPPER_VENDOR_ID");
    uint32_t driverId = compatEnvUint("VORTEK_DRIVER_ID", "WRAPPER_DRIVER_ID");
    if (deviceName) snprintf(properties->deviceName, sizeof(properties->deviceName),
                             "Vortek (%s)", deviceName);
    if (deviceId) properties->deviceID = deviceId;
    if (vendorId) properties->vendorID = vendorId;
    if (compat->driverId == VK_DRIVER_ID_SAMSUNG_PROPRIETARY &&
        properties->limits.maxPushConstantsSize < 256)
        properties->limits.maxPushConstantsSize = 256;

    for (VkBaseOutStructure* item = pNext; item; item = item->pNext) {
        switch (item->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR:
                if (isEmulatedExtension(compat, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
                    ((VkPhysicalDevicePushDescriptorPropertiesKHR*)item)
                        ->maxPushDescriptors = VORTEK_MAX_PUSH_DESCRIPTORS;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR:
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT:
                if (compat->driverId == VK_DRIVER_ID_ARM_PROPRIETARY)
                    ((VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR*)item)
                        ->maxVertexAttribDivisor = UINT32_MAX;
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES:
                if (compat->isD3D)
                    BUMP_UAB_LIMIT(
                        (VkPhysicalDeviceDescriptorIndexingProperties*)item,
                        VORTEK_D3D_BINDLESS_LIMIT);
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES: {
                VkPhysicalDeviceVulkan12Properties* vk12 = (void*)item;
                if (compat->driverId == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
                    vk12->denormBehaviorIndependence =
                        VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
                    vk12->roundingModeIndependence =
                        VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
                    vk12->shaderDenormFlushToZeroFloat16 = VK_FALSE;
                    vk12->shaderDenormFlushToZeroFloat32 = VK_FALSE;
                    vk12->shaderRoundingModeRTEFloat16 = VK_FALSE;
                    vk12->shaderRoundingModeRTEFloat32 = VK_FALSE;
                    vk12->shaderSignedZeroInfNanPreserveFloat16 = VK_FALSE;
                    vk12->shaderSignedZeroInfNanPreserveFloat32 = VK_FALSE;
                }
                if (driverId) vk12->driverID = (VkDriverId)driverId;
                if (compat->isD3D)
                    BUMP_UAB_LIMIT(vk12, VORTEK_D3D_BINDLESS_LIMIT);
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES_KHR:
                if (compat->driverId == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
                    VkPhysicalDeviceFloatControlsPropertiesKHR* controls =
                        (void*)item;
                    controls->denormBehaviorIndependence =
                        VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
                    controls->roundingModeIndependence =
                        VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
                    controls->shaderDenormFlushToZeroFloat16 = VK_FALSE;
                    controls->shaderDenormFlushToZeroFloat32 = VK_FALSE;
                    controls->shaderRoundingModeRTEFloat16 = VK_FALSE;
                    controls->shaderRoundingModeRTEFloat32 = VK_FALSE;
                    controls->shaderSignedZeroInfNanPreserveFloat16 = VK_FALSE;
                    controls->shaderSignedZeroInfNanPreserveFloat32 = VK_FALSE;
                }
                break;
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES: {
                VkPhysicalDeviceSubgroupProperties* subgroup = (void*)item;
                subgroup->supportedOperations = 0;
                subgroup->supportedStages = 0;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES: {
                VkPhysicalDeviceVulkan11Properties* vk11 = (void*)item;
                vk11->subgroupSupportedOperations = 0;
                vk11->subgroupSupportedStages = 0;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES_EXT: {
                VkPhysicalDeviceTexelBufferAlignmentPropertiesEXT* alignment =
                    (void*)item;
                alignment->storageTexelBufferOffsetAlignmentBytes = 1;
                alignment->uniformTexelBufferOffsetAlignmentBytes = 1;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES: {
                VkPhysicalDeviceVulkan13Properties* vk13 = (void*)item;
                vk13->storageTexelBufferOffsetAlignmentBytes = 1;
                vk13->uniformTexelBufferOffsetAlignmentBytes = 1;
                break;
            }
            default:
                break;
        }
    }
}

void VortekWrapperCompat_prepareDeviceCreateInfo(VkContext* context,
                                                 VkDeviceCreateInfo* createInfo) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (!compat) return;
    compat->emulatePushDescriptors =
        extensionEnabled(createInfo, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) &&
        isEmulatedExtension(compat, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    aliasVertexDivisorExtension(context, createInfo);
    removeEmulatedExtensions(context, createInfo);
    filterEmulatedFeatureStructures(compat, createInfo);
    addDeviceFaultFeature(context, createInfo);
}

bool VortekWrapperCompat_safeCreateEnabled(VkContext* context) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    return compat && compat->safeCreate;
}

static void createNullResources(VortekWrapperCompat* compat, VkDevice device) {
    VkBufferCreateInfo bufferInfo = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 65536,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vulkanWrapper.vkCreateBuffer(device, &bufferInfo, NULL,
                                     &compat->nullBuffer) != VK_SUCCESS)
        return;
    VkMemoryRequirements requirements = {0};
    vulkanWrapper.vkGetBufferMemoryRequirements(device, compat->nullBuffer,
                                                &requirements);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = getMemoryTypeIndex(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    if (vulkanWrapper.vkAllocateMemory(device, &allocation, NULL,
                                       &compat->nullBufferMemory) != VK_SUCCESS)
        return;
    vulkanWrapper.vkBindBufferMemory(device, compat->nullBuffer,
                                     compat->nullBufferMemory, 0);
    void* mapped = NULL;
    if (vulkanWrapper.vkMapMemory(device, compat->nullBufferMemory, 0,
                                  VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) {
        memset(mapped, 0, (size_t)requirements.size);
        vulkanWrapper.vkUnmapMemory(device, compat->nullBufferMemory);
    }

    VkImageCreateInfo imageInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = {1, 1, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vulkanWrapper.vkCreateImage(device, &imageInfo, NULL,
                                    &compat->nullImage) != VK_SUCCESS)
        return;
    vulkanWrapper.vkGetImageMemoryRequirements(device, compat->nullImage,
                                               &requirements);
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = getMemoryTypeIndex(requirements.memoryTypeBits, 0);
    if (vulkanWrapper.vkAllocateMemory(device, &allocation, NULL,
                                       &compat->nullImageMemory) != VK_SUCCESS)
        return;
    vulkanWrapper.vkBindImageMemory(device, compat->nullImage,
                                    compat->nullImageMemory, 0);
    VkImageViewCreateInfo viewInfo = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = compat->nullImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vulkanWrapper.vkCreateImageView(device, &viewInfo, NULL,
                                    &compat->nullImageView);
    VkSamplerCreateInfo samplerInfo = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .maxLod = 0.0f,
    };
    vulkanWrapper.vkCreateSampler(device, &samplerInfo, NULL,
                                  &compat->nullSampler);
}

void VortekWrapperCompat_deviceCreated(VkContext* context, VkDevice device,
                                       bool usedPNext) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (!compat) return;
    compat->device = device;
    compat->deviceFaultEnabled = usedPNext && compat->deviceFaultRequested &&
        nativeExtensionSupported(compat, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) &&
        compat->baseDeviceFault;
    if (compat->deviceFaultEnabled)
        compat->getDeviceFaultInfo = (PFN_vkGetDeviceFaultInfoEXT)
            vulkanWrapper.vkGetDeviceProcAddr(device, "vkGetDeviceFaultInfoEXT");
    if (compat->emulateNullDescriptor && !compat->baseNullDescriptor)
        createNullResources(compat, device);
}

static void destroyNullResources(VortekWrapperCompat* compat, VkDevice device) {
    if (compat->nullSampler)
        vulkanWrapper.vkDestroySampler(device, compat->nullSampler, NULL);
    if (compat->nullImageView)
        vulkanWrapper.vkDestroyImageView(device, compat->nullImageView, NULL);
    if (compat->nullImage)
        vulkanWrapper.vkDestroyImage(device, compat->nullImage, NULL);
    if (compat->nullImageMemory)
        vulkanWrapper.vkFreeMemory(device, compat->nullImageMemory, NULL);
    if (compat->nullBuffer)
        vulkanWrapper.vkDestroyBuffer(device, compat->nullBuffer, NULL);
    if (compat->nullBufferMemory)
        vulkanWrapper.vkFreeMemory(device, compat->nullBufferMemory, NULL);
    compat->nullSampler = VK_NULL_HANDLE;
    compat->nullImageView = VK_NULL_HANDLE;
    compat->nullImage = VK_NULL_HANDLE;
    compat->nullImageMemory = VK_NULL_HANDLE;
    compat->nullBuffer = VK_NULL_HANDLE;
    compat->nullBufferMemory = VK_NULL_HANDLE;
}

void VortekWrapperCompat_destroyDevice(VkContext* context, VkDevice device) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (!compat) return;
    clearTracking(compat, device);
    destroyNullResources(compat, device);
    compat->device = VK_NULL_HANDLE;
    compat->deviceFaultEnabled = false;
    compat->getDeviceFaultInfo = NULL;
}

static FILE* openDiagFile(void) {
    const char* path = compatEnv("VORTEK_DIAG_FILE", "WRAPPER_DIAG_FILE");
    return path ? fopen(path, "a") : NULL;
}

static void diagPrint(FILE* file, const char* format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    fprintf(stderr, "[VORTEK_DIAG] ");
    vfprintf(stderr, format, args);
    if (file) {
        fprintf(file, "[VORTEK_DIAG] ");
        vfprintf(file, format, copy);
        fflush(file);
    }
    va_end(copy);
    va_end(args);
}

void VortekWrapperCompat_emitDeviceReport(VkContext* context,
                                          const VkDeviceCreateInfo* createInfo,
                                          VkResult result, bool usedFallback) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (!compat || !compat->diagnostics) return;
    FILE* file = openDiagFile();
    VkPhysicalDeviceProperties properties = {0};
    VkPhysicalDeviceFeatures features = {0};
    vulkanWrapper.vkGetPhysicalDeviceProperties(compat->physicalDevice, &properties);
    vulkanWrapper.vkGetPhysicalDeviceFeatures(compat->physicalDevice, &features);
    diagPrint(file, "==== device report ====\n");
    diagPrint(file, "app=%s engine=%s engineVersion=%u.%u.%u\n",
              compat->applicationName[0] ? compat->applicationName : "(unknown)",
              compat->engineName[0] ? compat->engineName : "(unknown)",
              VK_VERSION_MAJOR(compat->engineVersion),
              VK_VERSION_MINOR(compat->engineVersion),
              VK_VERSION_PATCH(compat->engineVersion));
    diagPrint(file, "gpu=%s driverID=%d driverVersion=%u api=%u.%u.%u\n",
              properties.deviceName, compat->driverId, properties.driverVersion,
              VK_VERSION_MAJOR(properties.apiVersion),
              VK_VERSION_MINOR(properties.apiVersion),
              VK_VERSION_PATCH(properties.apiVersion));
    diagPrint(file, "CreateDevice=%d fallbackWithoutPNext=%s\n", result,
              usedFallback ? "yes" : "no");
    diagPrint(file, "features: BC=%u nullDescriptor(base)=%u dualSrcBlend=%u multiDrawIndirect=%u\n",
              features.textureCompressionBC, compat->baseNullDescriptor,
              features.dualSrcBlend, features.multiDrawIndirect);
    for (uint32_t i = 0; createInfo && i < createInfo->enabledExtensionCount; i++)
        diagPrint(file, "enabledExtension[%u]=%s\n", i,
                  createInfo->ppEnabledExtensionNames[i]);
    diagPrint(file, "=======================\n");
    if (file) fclose(file);
}

static const char* faultAddressType(VkDeviceFaultAddressTypeEXT type) {
    switch (type) {
        case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT: return "none";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT: return "read-invalid";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT: return "write-invalid";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT: return "execute-invalid";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT: return "ip-unknown";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT: return "ip-invalid";
        case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT: return "ip-fault";
        default: return "unknown";
    }
}

void VortekWrapperCompat_logDeviceFault(VkContext* context) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (!compat || !compat->diagnostics || !compat->deviceFaultEnabled ||
        !compat->getDeviceFaultInfo || !compat->device)
        return;
    VkDeviceFaultCountsEXT counts = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT,
    };
    if (compat->getDeviceFaultInfo(compat->device, &counts, NULL) != VK_SUCCESS)
        return;
    VkDeviceFaultInfoEXT info = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,
    };
    if (counts.addressInfoCount)
        info.pAddressInfos = calloc(counts.addressInfoCount,
                                    sizeof(*info.pAddressInfos));
    if (counts.vendorInfoCount)
        info.pVendorInfos = calloc(counts.vendorInfoCount,
                                   sizeof(*info.pVendorInfos));
    counts.vendorBinarySize = 0;
    FILE* file = openDiagFile();
    if (compat->getDeviceFaultInfo(compat->device, &counts, &info) == VK_SUCCESS) {
        diagPrint(file, "==== VK_ERROR_DEVICE_LOST: GPU fault ====\n");
        diagPrint(file, "description=%s\n", info.description);
        for (uint32_t i = 0; i < counts.addressInfoCount; i++) {
            const VkDeviceFaultAddressInfoEXT* address = &info.pAddressInfos[i];
            diagPrint(file, "address[%u] type=%s address=0x%llx precision=0x%llx\n",
                      i, faultAddressType(address->addressType),
                      (unsigned long long)address->reportedAddress,
                      (unsigned long long)address->addressPrecision);
        }
        for (uint32_t i = 0; i < counts.vendorInfoCount; i++) {
            const VkDeviceFaultVendorInfoEXT* vendor = &info.pVendorInfos[i];
            diagPrint(file, "vendor[%u] %s code=0x%llx data=0x%llx\n", i,
                      vendor->description,
                      (unsigned long long)vendor->vendorFaultCode,
                      (unsigned long long)vendor->vendorFaultData);
        }
    }
    if (file) fclose(file);
    free(info.pAddressInfos);
    free(info.pVendorInfos);
}

static PushDescriptorLayout* findPushLayout(VortekWrapperCompat* compat,
                                            VkDescriptorSetLayout layout) {
    for (int i = 0; i < compat->pushLayouts.size; i++) {
        PushDescriptorLayout* current = compat->pushLayouts.elements[i];
        if (current->layout == layout) return current;
    }
    return NULL;
}

static PushPipelineLayout* findPipelineLayout(VortekWrapperCompat* compat,
                                              VkPipelineLayout layout) {
    for (int i = 0; i < compat->pipelineLayouts.size; i++) {
        PushPipelineLayout* current = compat->pipelineLayouts.elements[i];
        if (current->layout == layout) return current;
    }
    return NULL;
}

static PushCommandBuffer* findCommandBuffer(VortekWrapperCompat* compat,
                                            VkCommandBuffer commandBuffer) {
    for (int i = 0; i < compat->commandBuffers.size; i++) {
        PushCommandBuffer* current = compat->commandBuffers.elements[i];
        if (current->commandBuffer == commandBuffer) return current;
    }
    return NULL;
}

static void addPoolSize(PushDescriptorLayout* record, VkDescriptorType type,
                        uint32_t descriptorCount) {
    for (uint32_t i = 0; i < record->sizeCount; i++) {
        if (record->sizes[i].type == type) {
            record->sizes[i].descriptorCount += descriptorCount;
            return;
        }
    }
    if (record->sizeCount < VORTEK_MAX_PUSH_POOL_SIZES) {
        record->sizes[record->sizeCount++] = (VkDescriptorPoolSize){
            .type = type,
            .descriptorCount = descriptorCount,
        };
    }
}

VkResult VortekWrapperCompat_createDescriptorSetLayout(
    VkContext* context, VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* createInfo,
    VkDescriptorSetLayout* setLayout) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    bool emulate = compat && compat->emulatePushDescriptors &&
        (createInfo->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR);
    VkDescriptorSetLayoutCreateInfo nativeInfo = *createInfo;
    if (emulate)
        nativeInfo.flags &= ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    VkResult result = vulkanWrapper.vkCreateDescriptorSetLayout(
        device, &nativeInfo, NULL, setLayout);
    if (result != VK_SUCCESS || !emulate) return result;

    PushDescriptorLayout* record = calloc(1, sizeof(*record));
    if (!record) return result;
    record->layout = *setLayout;
    if (createInfo->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT)
        record->poolFlags |= VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    for (uint32_t i = 0; i < createInfo->bindingCount; i++)
        addPoolSize(record, createInfo->pBindings[i].descriptorType,
                    createInfo->pBindings[i].descriptorCount);
    ArrayList_add(&compat->pushLayouts, record);
    return result;
}

void VortekWrapperCompat_destroyDescriptorSetLayout(VkContext* context,
                                                    VkDevice device,
                                                    VkDescriptorSetLayout setLayout) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (compat) {
        for (int i = compat->pushLayouts.size - 1; i >= 0; i--) {
            PushDescriptorLayout* record = compat->pushLayouts.elements[i];
            if (record->layout == setLayout) {
                ArrayList_removeAt(&compat->pushLayouts, i);
                free(record);
            }
        }
    }
    vulkanWrapper.vkDestroyDescriptorSetLayout(device, setLayout, NULL);
}

VkResult VortekWrapperCompat_createPipelineLayout(
    VkContext* context, VkDevice device,
    const VkPipelineLayoutCreateInfo* createInfo,
    VkPipelineLayout* pipelineLayout) {
    VkResult result = vulkanWrapper.vkCreatePipelineLayout(
        device, createInfo, NULL, pipelineLayout);
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (result != VK_SUCCESS || !compat || !compat->emulatePushDescriptors)
        return result;
    PushPipelineLayout* record = calloc(1, sizeof(*record));
    if (!record) return result;
    record->layout = *pipelineLayout;
    record->setLayoutCount = createInfo->setLayoutCount;
    if (record->setLayoutCount) {
        record->setLayouts = malloc(record->setLayoutCount * sizeof(*record->setLayouts));
        if (!record->setLayouts) {
            free(record);
            return result;
        }
        memcpy(record->setLayouts, createInfo->pSetLayouts,
               record->setLayoutCount * sizeof(*record->setLayouts));
    }
    ArrayList_add(&compat->pipelineLayouts, record);
    return result;
}

void VortekWrapperCompat_destroyPipelineLayout(VkContext* context,
                                               VkDevice device,
                                               VkPipelineLayout pipelineLayout) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (compat) {
        for (int i = compat->pipelineLayouts.size - 1; i >= 0; i--) {
            PushPipelineLayout* record = compat->pipelineLayouts.elements[i];
            if (record->layout == pipelineLayout) {
                ArrayList_removeAt(&compat->pipelineLayouts, i);
                MEMFREE(record->setLayouts);
                free(record);
            }
        }
    }
    vulkanWrapper.vkDestroyPipelineLayout(device, pipelineLayout, NULL);
}

void VortekWrapperCompat_trackCommandBuffers(VkContext* context, VkDevice device,
                                             const VkCommandBufferAllocateInfo* allocateInfo,
                                             const VkCommandBuffer* commandBuffers) {
    (void)device;
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (!compat || !compat->emulatePushDescriptors) return;
    for (uint32_t i = 0; i < allocateInfo->commandBufferCount; i++) {
        PushCommandBuffer* record = calloc(1, sizeof(*record));
        if (!record) continue;
        record->commandBuffer = commandBuffers[i];
        record->commandPool = allocateInfo->commandPool;
        ArrayList_add(&compat->commandBuffers, record);
    }
}

static void removeCommandBufferAt(VortekWrapperCompat* compat, VkDevice device,
                                  int index) {
    PushCommandBuffer* record = compat->commandBuffers.elements[index];
    freePushPools(device, record->pools);
    ArrayList_removeAt(&compat->commandBuffers, index);
    free(record);
}

void VortekWrapperCompat_freeCommandBuffers(VkContext* context, VkDevice device,
                                            VkCommandPool commandPool,
                                            uint32_t commandBufferCount,
                                            const VkCommandBuffer* commandBuffers) {
    (void)commandPool;
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (!compat) return;
    for (uint32_t j = 0; j < commandBufferCount; j++) {
        for (int i = compat->commandBuffers.size - 1; i >= 0; i--) {
            PushCommandBuffer* record = compat->commandBuffers.elements[i];
            if (record->commandBuffer == commandBuffers[j])
                removeCommandBufferAt(compat, device, i);
        }
    }
}

static void resetPushPools(VortekWrapperCompat* compat,
                           PushCommandBuffer* command) {
    for (PushDescriptorPool* pool = command ? command->pools : NULL;
         pool; pool = pool->next) {
        if (vulkanWrapper.vkResetDescriptorPool(compat->device, pool->pool, 0) ==
            VK_SUCCESS)
            pool->remaining = VORTEK_PUSH_POOL_CHUNK;
    }
}

void VortekWrapperCompat_resetCommandBuffer(VkContext* context,
                                            VkCommandBuffer commandBuffer) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (compat) resetPushPools(compat, findCommandBuffer(compat, commandBuffer));
}

void VortekWrapperCompat_resetCommandPool(VkContext* context,
                                          VkCommandPool commandPool) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (!compat) return;
    for (int i = 0; i < compat->commandBuffers.size; i++) {
        PushCommandBuffer* command = compat->commandBuffers.elements[i];
        if (command->commandPool == commandPool) resetPushPools(compat, command);
    }
}

void VortekWrapperCompat_destroyCommandPool(VkContext* context, VkDevice device,
                                            VkCommandPool commandPool) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (!compat) return;
    for (int i = compat->commandBuffers.size - 1; i >= 0; i--) {
        PushCommandBuffer* command = compat->commandBuffers.elements[i];
        if (command->commandPool == commandPool)
            removeCommandBufferAt(compat, device, i);
    }
}

static void patchedUpdateDescriptorSets(
    VortekWrapperCompat* compat, VkDevice device, uint32_t writeCount,
    const VkWriteDescriptorSet* sourceWrites, uint32_t copyCount,
    const VkCopyDescriptorSet* copies) {
    if (!compat || !compat->emulateNullDescriptor || writeCount == 0 ||
        (!compat->nullBuffer && !compat->nullImageView && !compat->nullSampler)) {
        vulkanWrapper.vkUpdateDescriptorSets(device, writeCount, sourceWrites,
                                             copyCount, copies);
        return;
    }
    VkWriteDescriptorSet* writes = malloc(writeCount * sizeof(*writes));
    if (!writes) return;
    memcpy(writes, sourceWrites, writeCount * sizeof(*writes));
    for (uint32_t i = 0; i < writeCount; i++) {
        uint32_t count = sourceWrites[i].descriptorCount;
        if (IS_DESCRIPTOR_BUFFER_INFO(sourceWrites[i].descriptorType) &&
            sourceWrites[i].pBufferInfo) {
            VkDescriptorBufferInfo* infos = malloc(count * sizeof(*infos));
            if (!infos) continue;
            memcpy(infos, sourceWrites[i].pBufferInfo, count * sizeof(*infos));
            for (uint32_t j = 0; j < count; j++) {
                if (!infos[j].buffer && compat->nullBuffer) {
                    infos[j].buffer = compat->nullBuffer;
                    infos[j].offset = 0;
                    infos[j].range = VK_WHOLE_SIZE;
                }
            }
            writes[i].pBufferInfo = infos;
        } else if (IS_DESCRIPTOR_IMAGE_INFO(sourceWrites[i].descriptorType) &&
                   sourceWrites[i].pImageInfo) {
            VkDescriptorImageInfo* infos = malloc(count * sizeof(*infos));
            if (!infos) continue;
            memcpy(infos, sourceWrites[i].pImageInfo, count * sizeof(*infos));
            for (uint32_t j = 0; j < count; j++) {
                if (sourceWrites[i].descriptorType != VK_DESCRIPTOR_TYPE_SAMPLER &&
                    !infos[j].imageView && compat->nullImageView) {
                    infos[j].imageView = compat->nullImageView;
                    infos[j].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                }
                if ((sourceWrites[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER ||
                     sourceWrites[i].descriptorType ==
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) &&
                    !infos[j].sampler && compat->nullSampler)
                    infos[j].sampler = compat->nullSampler;
            }
            writes[i].pImageInfo = infos;
        }
    }
    vulkanWrapper.vkUpdateDescriptorSets(device, writeCount, writes, copyCount,
                                         copies);
    for (uint32_t i = 0; i < writeCount; i++) {
        if (writes[i].pBufferInfo != sourceWrites[i].pBufferInfo)
            free((void*)writes[i].pBufferInfo);
        if (writes[i].pImageInfo != sourceWrites[i].pImageInfo)
            free((void*)writes[i].pImageInfo);
    }
    free(writes);
}

void VortekWrapperCompat_updateDescriptorSets(
    VkContext* context, VkDevice device, uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* descriptorWrites,
    uint32_t descriptorCopyCount, const VkCopyDescriptorSet* descriptorCopies) {
    patchedUpdateDescriptorSets(context ? context->wrapperCompat : NULL, device,
                                descriptorWriteCount, descriptorWrites,
                                descriptorCopyCount, descriptorCopies);
}

static VkResult allocatePushSet(VortekWrapperCompat* compat,
                                PushCommandBuffer* command,
                                PushDescriptorLayout* layout,
                                VkDescriptorSet* descriptorSet) {
    for (PushDescriptorPool* pool = command->pools; pool; pool = pool->next) {
        if (pool->layout != layout->layout || pool->remaining == 0) continue;
        VkDescriptorSetAllocateInfo allocation = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = pool->pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &layout->layout,
        };
        VkResult result = vulkanWrapper.vkAllocateDescriptorSets(
            compat->device, &allocation, descriptorSet);
        if (result == VK_SUCCESS) {
            pool->remaining--;
            return result;
        }
        pool->remaining = 0;
    }

    VkDescriptorPoolSize sizes[VORTEK_MAX_PUSH_POOL_SIZES];
    for (uint32_t i = 0; i < layout->sizeCount; i++) {
        sizes[i] = layout->sizes[i];
        sizes[i].descriptorCount *= VORTEK_PUSH_POOL_CHUNK;
    }
    VkDescriptorPoolCreateInfo createInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = layout->poolFlags,
        .maxSets = VORTEK_PUSH_POOL_CHUNK,
        .poolSizeCount = layout->sizeCount,
        .pPoolSizes = sizes,
    };
    PushDescriptorPool* pool = calloc(1, sizeof(*pool));
    if (!pool) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult result = vulkanWrapper.vkCreateDescriptorPool(
        compat->device, &createInfo, NULL, &pool->pool);
    if (result != VK_SUCCESS) {
        free(pool);
        return result;
    }
    pool->layout = layout->layout;
    pool->remaining = VORTEK_PUSH_POOL_CHUNK;
    pool->next = command->pools;
    command->pools = pool;

    VkDescriptorSetAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool->pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout->layout,
    };
    result = vulkanWrapper.vkAllocateDescriptorSets(
        compat->device, &allocation, descriptorSet);
    if (result == VK_SUCCESS) pool->remaining--;
    return result;
}

void VortekWrapperCompat_cmdPushDescriptorSet(
    VkContext* context, VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
    uint32_t set, uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* descriptorWrites) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    if (!compat || !compat->emulatePushDescriptors) {
        if (vulkanWrapper.vkCmdPushDescriptorSet)
            vulkanWrapper.vkCmdPushDescriptorSet(
                commandBuffer, pipelineBindPoint, layout, set,
                descriptorWriteCount, descriptorWrites);
        return;
    }
    PushPipelineLayout* pipeline = findPipelineLayout(compat, layout);
    PushCommandBuffer* command = findCommandBuffer(compat, commandBuffer);
    if (!pipeline || !command || set >= pipeline->setLayoutCount) return;
    PushDescriptorLayout* descriptorLayout =
        findPushLayout(compat, pipeline->setLayouts[set]);
    if (!descriptorLayout) return;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    if (allocatePushSet(compat, command, descriptorLayout, &descriptorSet) !=
        VK_SUCCESS)
        return;
    VkWriteDescriptorSet* writes = malloc(descriptorWriteCount * sizeof(*writes));
    if (!writes) return;
    memcpy(writes, descriptorWrites, descriptorWriteCount * sizeof(*writes));
    for (uint32_t i = 0; i < descriptorWriteCount; i++)
        writes[i].dstSet = descriptorSet;
    patchedUpdateDescriptorSets(compat, compat->device, descriptorWriteCount,
                                writes, 0, NULL);
    vulkanWrapper.vkCmdBindDescriptorSets(
        commandBuffer, pipelineBindPoint, layout, set, 1, &descriptorSet, 0, NULL);
    free(writes);
}

void VortekWrapperCompat_cmdBindVertexBuffers(
    VkContext* context, VkCommandBuffer commandBuffer, uint32_t firstBinding,
    uint32_t bindingCount, const VkBuffer* buffers,
    const VkDeviceSize* offsets) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    VkBuffer* patched = NULL;
    if (compat && compat->emulateNullDescriptor && compat->nullBuffer && buffers) {
        patched = malloc(bindingCount * sizeof(*patched));
        if (patched) {
            for (uint32_t i = 0; i < bindingCount; i++)
                patched[i] = buffers[i] ? buffers[i] : compat->nullBuffer;
        }
    }
    vulkanWrapper.vkCmdBindVertexBuffers(commandBuffer, firstBinding,
                                         bindingCount,
                                         patched ? patched : buffers, offsets);
    free(patched);
}

void VortekWrapperCompat_cmdBindVertexBuffers2(
    VkContext* context, VkCommandBuffer commandBuffer, uint32_t firstBinding,
    uint32_t bindingCount, const VkBuffer* buffers,
    const VkDeviceSize* offsets, const VkDeviceSize* sizes,
    const VkDeviceSize* strides) {
    VortekWrapperCompat* compat = context ? context->wrapperCompat : NULL;
    VkBuffer* patched = NULL;
    if (compat && compat->emulateNullDescriptor && compat->nullBuffer && buffers) {
        patched = malloc(bindingCount * sizeof(*patched));
        if (patched) {
            for (uint32_t i = 0; i < bindingCount; i++)
                patched[i] = buffers[i] ? buffers[i] : compat->nullBuffer;
        }
    }
    vulkanWrapper.vkCmdBindVertexBuffers2(commandBuffer, firstBinding,
                                          bindingCount,
                                          patched ? patched : buffers,
                                          offsets, sizes, strides);
    free(patched);
}
