#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdint>

// Stable C ABI used between Toolscreen.dll and the process-local Vulkan layer.
// The layer deliberately resolves these exports at runtime so it has no loader
// dependency on the injected DLL and fails closed when Toolscreen is absent.
struct ToolscreenVulkanObsComposition {
    VkImage image = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t slot = UINT32_MAX;
    uint64_t serial = 0;
    uint64_t frameSerial = 0;
};

extern "C" {

__declspec(dllexport) bool ToolscreenVulkanGetObsComposition(
    VkDevice device, VkImage presentedSwapchainImage, VkFormat format,
    VkExtent2D extent, ToolscreenVulkanObsComposition* result);

__declspec(dllexport) void ToolscreenVulkanObsRedirectSubmitted(
    uint32_t slot, uint64_t serial, VkFence fence);

__declspec(dllexport) void ToolscreenVulkanObsRedirectRetired(
    uint32_t slot, uint64_t serial, VkFence fence, VkResult completion);

__declspec(dllexport) void ToolscreenVulkanObsRedirectAbandoned(
    uint32_t slot, uint64_t serial, const char* reason);

__declspec(dllexport) void ToolscreenVulkanObsCaptureAvailability(
    VkDevice device, bool available, VkImage exportImage, VkFormat format,
    VkExtent2D extent);

__declspec(dllexport) void ToolscreenVulkanBeforeLowerDeviceDestroy(
    VkDevice device);

__declspec(dllexport) void ToolscreenVulkanLayerLog(const char* message);

}
