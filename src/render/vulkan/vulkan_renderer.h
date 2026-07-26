#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <vector>

namespace VulkanHooks {
struct DeviceDispatch;
struct ImageMetadata;
struct SwapchainMetadata;
}

namespace VulkanRenderer {

struct FinalBlitContext {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage sourceImage = VK_NULL_HANDLE;
    VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage destinationImage = VK_NULL_HANDLE;
    VkImageLayout destinationLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t regionCount = 0;
    const VkImageBlit* regions = nullptr;
    VkFilter filter = VK_FILTER_NEAREST;
    const VulkanHooks::DeviceDispatch* dispatch = nullptr;
    const VulkanHooks::ImageMetadata* sourceMetadata = nullptr;
    const VulkanHooks::ImageMetadata* destinationMetadata = nullptr;
    const VulkanHooks::SwapchainMetadata* swapchain = nullptr;
};

// Returns true when the original blit was emitted by this function.
bool RecordAfterFinalBlit(const FinalBlitContext& context, PFN_vkCmdBlitImage originalBlit);
bool IsReady();
// The color picker uses the current source descriptor for its live GPU
// preview. Pixel selection is a one-texel asynchronous transfer; results are
// exposed only after the frame's completion query becomes available.
bool GetColorPickerFrame(uintptr_t& textureId, int& width, int& height);
void RequestColorPickerSample(int x, int y);
bool TryGetColorPickerSample(int x, int y, std::array<float, 4>& color);
void OnQueueSubmit(VkDevice device, VkQueue queue, uint32_t commandBufferCount,
                   const VkCommandBuffer* commandBuffers, VkFence fence);
void OnImageDestroyed(VkDevice device, VkImage image);
void OnSwapchainDestroyed(VkDevice device, VkSwapchainKHR swapchain, const std::vector<VkImage>& images);
void OnDeviceDestroyed(VkDevice device);
void Shutdown();

} // namespace VulkanRenderer
