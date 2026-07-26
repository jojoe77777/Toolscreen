#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace VulkanHooks {

struct DeviceDispatch {
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr = nullptr;
    PFN_vkGetDeviceQueue getDeviceQueue = nullptr;
    PFN_vkGetDeviceQueue2 getDeviceQueue2 = nullptr;
    PFN_vkCreateSwapchainKHR createSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR getSwapchainImagesKHR = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkCreateImageView createImageView = nullptr;
    PFN_vkDestroyImageView destroyImageView = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers freeCommandBuffers = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkQueueSubmit2 queueSubmit2 = nullptr;
    PFN_vkQueueSubmit2KHR queueSubmit2KHR = nullptr;
    PFN_vkGetFenceStatus getFenceStatus = nullptr;
    PFN_vkCreateFence createFence = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkCreateSemaphore createSemaphore = nullptr;
    PFN_vkDestroySemaphore destroySemaphore = nullptr;
    PFN_vkGetQueryPoolResults getQueryPoolResults = nullptr;
    PFN_vkCreateQueryPool createQueryPool = nullptr;
    PFN_vkDestroyQueryPool destroyQueryPool = nullptr;
    PFN_vkCreateDescriptorPool createDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool destroyDescriptorPool = nullptr;
    PFN_vkCmdBlitImage cmdBlitImage = nullptr;
    PFN_vkCmdPipelineBarrier cmdPipelineBarrier = nullptr;
    PFN_vkCmdPipelineBarrier2 cmdPipelineBarrier2 = nullptr;
    PFN_vkCmdPipelineBarrier2KHR cmdPipelineBarrier2KHR = nullptr;
    PFN_vkCmdBeginRendering cmdBeginRendering = nullptr;
    PFN_vkCmdBeginRenderingKHR cmdBeginRenderingKHR = nullptr;
    PFN_vkCmdEndRendering cmdEndRendering = nullptr;
    PFN_vkCmdEndRenderingKHR cmdEndRenderingKHR = nullptr;
    PFN_vkCmdResetQueryPool cmdResetQueryPool = nullptr;
    PFN_vkCmdWriteTimestamp cmdWriteTimestamp = nullptr;
    PFN_vkCmdWriteTimestamp2 cmdWriteTimestamp2 = nullptr;
    PFN_vkCmdWriteTimestamp2KHR cmdWriteTimestamp2KHR = nullptr;
    PFN_vkCreateSampler createSampler = nullptr;
    PFN_vkDestroySampler destroySampler = nullptr;
    PFN_vkCreateBuffer createBuffer = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements getBufferMemoryRequirements = nullptr;
    PFN_vkGetImageMemoryRequirements getImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory allocateMemory = nullptr;
    PFN_vkFreeMemory freeMemory = nullptr;
    PFN_vkBindBufferMemory bindBufferMemory = nullptr;
    PFN_vkBindImageMemory bindImageMemory = nullptr;
    PFN_vkMapMemory mapMemory = nullptr;
    PFN_vkUnmapMemory unmapMemory = nullptr;
    PFN_vkFlushMappedMemoryRanges flushMappedMemoryRanges = nullptr;
    PFN_vkCmdCopyBufferToImage cmdCopyBufferToImage = nullptr;
};

struct ImageMetadata {
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    VkImageUsageFlags usage = 0;
    VkImageType type = VK_IMAGE_TYPE_2D;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageTiling tiling = VK_IMAGE_TILING_OPTIMAL;
    bool swapchainImage = false;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
};

struct SwapchainMetadata {
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkSwapchainKHR oldSwapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
    uint32_t minImageCount = 0;
    std::vector<VkImage> images;
};

struct QueueMetadata {
    VkDevice device = VK_NULL_HANDLE;
    uint32_t familyIndex = VK_QUEUE_FAMILY_IGNORED;
    uint32_t queueIndex = 0;
};

struct TrackingSnapshot {
    VkInstance instance = VK_NULL_HANDLE;
    std::unordered_map<VkDevice, DeviceDispatch> devices;
    std::unordered_map<VkImage, ImageMetadata> images;
    std::unordered_map<VkSwapchainKHR, SwapchainMetadata> swapchains;
    std::unordered_map<VkImageView, VkImage> imageViews;
    std::unordered_map<VkQueue, QueueMetadata> queues;
    std::unordered_map<VkCommandPool, VkDevice> commandPools;
    std::unordered_map<VkCommandBuffer, VkDevice> commandBuffers;
    std::unordered_map<VkFence, VkDevice> fences;
    std::unordered_map<VkSemaphore, VkDevice> semaphores;
};

std::shared_ptr<const TrackingSnapshot> GetSnapshot();
bool InstallIfAvailable();
void NotifyModuleLoaded(HMODULE module);
FARPROC InterceptLoaderGetProcAddress(HMODULE module, LPCSTR name, FARPROC realFunction);
void Shutdown();

// Used by ImGui's Vulkan backend. It deliberately returns the real functions,
// not Toolscreen wrappers, so Toolscreen's own resource work cannot recurse.
PFN_vkVoidFunction LoadRealFunction(const char* name, void* userData);

} // namespace VulkanHooks
