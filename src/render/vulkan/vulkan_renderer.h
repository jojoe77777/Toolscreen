#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <string>
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
    uint32_t commandBufferQueueFamily = VK_QUEUE_FAMILY_IGNORED;
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

// GPU frame-completion tracking counters (cumulative), for diagnostics and the in-game tests.
struct FrameTrackingStats {
    uint64_t completedFrames = 0;
    // Frames whose query results read as available before their own work ran (the slot's previous use).
    uint64_t earlyAvailabilityFrames = 0;
    // Frames recorded while every timestamp slot was still in flight, so nothing tracked their resources.
    uint64_t untrackedFrames = 0;
    // Test probe: presents sampled, and how many found the just-submitted frame's slot reading as complete by
    // query availability while its own work had not finished.
    uint64_t probeSamples = 0;
    uint64_t probeStaleAvailability = 0;
};
FrameTrackingStats GetFrameTrackingStats();
void SetFrameTrackingProbeEnabled(bool enabled);

// Streaming-texture cache counts for keys starting with keyPrefix (e.g. "window:"). Render thread only.
struct StreamingTextureStats {
    size_t keys = 0;
    size_t slots = 0;
    int largestWidth = 0;
    // Texture resources (of any kind) retired and waiting for in-flight frames to finish.
    size_t retiredAwaitingGpu = 0;
};
StreamingTextureStats GetStreamingTextureStats(const std::string& keyPrefix);
// The color picker uses the current source descriptor for its live GPU
// preview. Pixel selection is a one-texel asynchronous transfer; results are
// exposed only after the frame's completion query becomes available.
bool GetColorPickerFrame(uintptr_t& textureId, int& width, int& height);
void RequestColorPickerSample(int x, int y);
bool TryGetColorPickerSample(int x, int y, std::array<float, 4>& color);
bool GetBundledGuiTexture(int resourceId, uintptr_t& textureId);
bool GetGuiRgbaTexture(const std::string& key, const unsigned char* pixels,
                       int width, int height, uint64_t generation,
                       uintptr_t& textureId);
void OnQueueSubmit(VkDevice device, VkQueue queue, uint32_t commandBufferCount,
                   const VkCommandBuffer* commandBuffers, VkFence fence);
void OnQueuePresent(VkDevice device, VkQueue queue, const VkPresentInfoKHR* presentInfo,
                    const std::vector<VkImage>& presentImages);
void OnImageDestroyed(VkDevice device, VkImage image);
void OnSwapchainDestroyed(VkDevice device, VkSwapchainKHR swapchain, const std::vector<VkImage>& images);
void OnDeviceDestroyed(VkDevice device);
void Shutdown();
// Render thread only. The latest content-detection result for a mirror; false if none has been read back yet.
bool GetMirrorHasContentForTests(const std::string& mirrorName, bool& hasContent);
// Render thread only. Requests the presented swapchain pixel at (x, y) in window pixels; true once a sample of that
// pixel has been read back.
bool TryGetPresentedPixelForTests(int x, int y, std::array<float, 4>& color);
// Render thread only. Copies the next presented frame (RGBA, top row first) for tests instead of to the clipboard.
void RequestFrameCaptureForTests();
bool TryGetFrameCaptureForTests(std::vector<uint8_t>& rgba, int& width, int& height);

} // namespace VulkanRenderer
