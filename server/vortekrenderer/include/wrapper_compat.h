#ifndef VORTEK_WRAPPER_COMPAT_H
#define VORTEK_WRAPPER_COMPAT_H

#include "vortek.h"

typedef struct VortekWrapperCompat VortekWrapperCompat;

void VortekWrapperCompat_init(VkContext* context, VkPhysicalDevice physicalDevice,
                              const VkApplicationInfo* applicationInfo);
void VortekWrapperCompat_destroy(VkContext* context);

const char* const* VortekWrapperCompat_getDisabledExtensions(VkContext* context,
                                                             uint32_t* count);
const char* const* VortekWrapperCompat_getEmulatedExtensions(VkContext* context,
                                                             uint32_t* count);

void VortekWrapperCompat_applyFeatures(VkContext* context,
                                       VkPhysicalDeviceFeatures* features,
                                       void* pNext);
void VortekWrapperCompat_applyProperties(VkContext* context,
                                         VkPhysicalDeviceProperties* properties,
                                         void* pNext);

void VortekWrapperCompat_prepareDeviceCreateInfo(VkContext* context,
                                                 VkDeviceCreateInfo* createInfo);
bool VortekWrapperCompat_safeCreateEnabled(VkContext* context);
void VortekWrapperCompat_deviceCreated(VkContext* context, VkDevice device,
                                       bool usedPNext);
void VortekWrapperCompat_destroyDevice(VkContext* context, VkDevice device);
void VortekWrapperCompat_emitDeviceReport(VkContext* context,
                                          const VkDeviceCreateInfo* createInfo,
                                          VkResult result, bool usedFallback);
void VortekWrapperCompat_logDeviceFault(VkContext* context);

VkResult VortekWrapperCompat_createDescriptorSetLayout(
    VkContext* context, VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* createInfo,
    VkDescriptorSetLayout* setLayout);
void VortekWrapperCompat_destroyDescriptorSetLayout(VkContext* context,
                                                    VkDevice device,
                                                    VkDescriptorSetLayout setLayout);
VkResult VortekWrapperCompat_createPipelineLayout(
    VkContext* context, VkDevice device,
    const VkPipelineLayoutCreateInfo* createInfo,
    VkPipelineLayout* pipelineLayout);
void VortekWrapperCompat_destroyPipelineLayout(VkContext* context,
                                               VkDevice device,
                                               VkPipelineLayout pipelineLayout);

void VortekWrapperCompat_trackCommandBuffers(VkContext* context, VkDevice device,
                                             const VkCommandBufferAllocateInfo* allocateInfo,
                                             const VkCommandBuffer* commandBuffers);
void VortekWrapperCompat_freeCommandBuffers(VkContext* context, VkDevice device,
                                            VkCommandPool commandPool,
                                            uint32_t commandBufferCount,
                                            const VkCommandBuffer* commandBuffers);
void VortekWrapperCompat_resetCommandBuffer(VkContext* context,
                                            VkCommandBuffer commandBuffer);
void VortekWrapperCompat_resetCommandPool(VkContext* context,
                                          VkCommandPool commandPool);
void VortekWrapperCompat_destroyCommandPool(VkContext* context, VkDevice device,
                                            VkCommandPool commandPool);

void VortekWrapperCompat_updateDescriptorSets(
    VkContext* context, VkDevice device, uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* descriptorWrites,
    uint32_t descriptorCopyCount, const VkCopyDescriptorSet* descriptorCopies);
void VortekWrapperCompat_cmdPushDescriptorSet(
    VkContext* context, VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
    uint32_t set, uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* descriptorWrites);
void VortekWrapperCompat_cmdBindVertexBuffers(
    VkContext* context, VkCommandBuffer commandBuffer, uint32_t firstBinding,
    uint32_t bindingCount, const VkBuffer* buffers,
    const VkDeviceSize* offsets);
void VortekWrapperCompat_cmdBindVertexBuffers2(
    VkContext* context, VkCommandBuffer commandBuffer, uint32_t firstBinding,
    uint32_t bindingCount, const VkBuffer* buffers,
    const VkDeviceSize* offsets, const VkDeviceSize* sizes,
    const VkDeviceSize* strides);

#endif
