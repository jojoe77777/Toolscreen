#include "vulkan_hooks.h"

#include "MinHook.h"
#include "common/profiler.h"
#include "common/utils.h"
#include "render/render_backend.h"
#include "render/vulkan/vulkan_renderer.h"
#include "version.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string_view>

namespace {

using namespace VulkanHooks;

std::atomic<std::shared_ptr<const TrackingSnapshot>> g_snapshot{ std::make_shared<const TrackingSnapshot>() };
std::mutex g_trackingWriteMutex;
std::atomic<bool> g_installed{ false };
std::atomic<bool> g_loggedFirstBlit{ false };
std::atomic<bool> g_loggedFinalBlitDecision{ false };

std::atomic<PFN_vkGetInstanceProcAddr> g_realGetInstanceProcAddr{ nullptr };
std::atomic<PFN_vkGetDeviceProcAddr> g_realGetDeviceProcAddr{ nullptr };
std::atomic<PFN_vkCreateInstance> g_realCreateInstance{ nullptr };
std::atomic<PFN_vkDestroyInstance> g_realDestroyInstance{ nullptr };
std::atomic<PFN_vkCreateDevice> g_realCreateDevice{ nullptr };
std::atomic<PFN_vkDestroyDevice> g_realDestroyDevice{ nullptr };
std::atomic<PFN_vkCreateWin32SurfaceKHR> g_realCreateWin32SurfaceKHR{ nullptr };
std::atomic<PFN_vkDestroySurfaceKHR> g_realDestroySurfaceKHR{ nullptr };

template <typename Fn>
void UpdateSnapshot(Fn&& fn) {
    std::lock_guard<std::mutex> lock(g_trackingWriteMutex);
    auto next = std::make_shared<TrackingSnapshot>(*g_snapshot.load(std::memory_order_acquire));
    fn(*next);
    g_snapshot.store(std::move(next), std::memory_order_release);
}

template <typename T>
T LoadDevice(PFN_vkGetDeviceProcAddr gdpa, VkDevice device, const char* name) {
    return gdpa ? reinterpret_cast<T>(gdpa(device, name)) : nullptr;
}

DeviceDispatch BuildDeviceDispatch(VkPhysicalDevice physicalDevice, VkDevice device, PFN_vkGetDeviceProcAddr gdpa) {
    DeviceDispatch d{};
    d.physicalDevice = physicalDevice;
    d.getDeviceProcAddr = gdpa;
#define LOAD_DEVICE(member, type, functionName) d.member = LoadDevice<type>(gdpa, device, functionName)
    LOAD_DEVICE(deviceWaitIdle, PFN_vkDeviceWaitIdle, "vkDeviceWaitIdle");
    LOAD_DEVICE(getDeviceQueue, PFN_vkGetDeviceQueue, "vkGetDeviceQueue");
    LOAD_DEVICE(getDeviceQueue2, PFN_vkGetDeviceQueue2, "vkGetDeviceQueue2");
    LOAD_DEVICE(createSwapchainKHR, PFN_vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
    LOAD_DEVICE(destroySwapchainKHR, PFN_vkDestroySwapchainKHR, "vkDestroySwapchainKHR");
    LOAD_DEVICE(getSwapchainImagesKHR, PFN_vkGetSwapchainImagesKHR, "vkGetSwapchainImagesKHR");
    LOAD_DEVICE(createImage, PFN_vkCreateImage, "vkCreateImage");
    LOAD_DEVICE(destroyImage, PFN_vkDestroyImage, "vkDestroyImage");
    LOAD_DEVICE(createImageView, PFN_vkCreateImageView, "vkCreateImageView");
    LOAD_DEVICE(destroyImageView, PFN_vkDestroyImageView, "vkDestroyImageView");
    LOAD_DEVICE(createCommandPool, PFN_vkCreateCommandPool, "vkCreateCommandPool");
    LOAD_DEVICE(destroyCommandPool, PFN_vkDestroyCommandPool, "vkDestroyCommandPool");
    LOAD_DEVICE(allocateCommandBuffers, PFN_vkAllocateCommandBuffers, "vkAllocateCommandBuffers");
    LOAD_DEVICE(freeCommandBuffers, PFN_vkFreeCommandBuffers, "vkFreeCommandBuffers");
    LOAD_DEVICE(queueSubmit, PFN_vkQueueSubmit, "vkQueueSubmit");
    LOAD_DEVICE(queueSubmit2, PFN_vkQueueSubmit2, "vkQueueSubmit2");
    LOAD_DEVICE(queueSubmit2KHR, PFN_vkQueueSubmit2KHR, "vkQueueSubmit2KHR");
    LOAD_DEVICE(queuePresentKHR, PFN_vkQueuePresentKHR, "vkQueuePresentKHR");
    LOAD_DEVICE(getFenceStatus, PFN_vkGetFenceStatus, "vkGetFenceStatus");
    LOAD_DEVICE(createFence, PFN_vkCreateFence, "vkCreateFence");
    LOAD_DEVICE(destroyFence, PFN_vkDestroyFence, "vkDestroyFence");
    LOAD_DEVICE(createSemaphore, PFN_vkCreateSemaphore, "vkCreateSemaphore");
    LOAD_DEVICE(destroySemaphore, PFN_vkDestroySemaphore, "vkDestroySemaphore");
    LOAD_DEVICE(getQueryPoolResults, PFN_vkGetQueryPoolResults, "vkGetQueryPoolResults");
    LOAD_DEVICE(createQueryPool, PFN_vkCreateQueryPool, "vkCreateQueryPool");
    LOAD_DEVICE(destroyQueryPool, PFN_vkDestroyQueryPool, "vkDestroyQueryPool");
    LOAD_DEVICE(createDescriptorPool, PFN_vkCreateDescriptorPool, "vkCreateDescriptorPool");
    LOAD_DEVICE(destroyDescriptorPool, PFN_vkDestroyDescriptorPool, "vkDestroyDescriptorPool");
    LOAD_DEVICE(cmdBlitImage, PFN_vkCmdBlitImage, "vkCmdBlitImage");
    LOAD_DEVICE(cmdResolveImage, PFN_vkCmdResolveImage, "vkCmdResolveImage");
    LOAD_DEVICE(cmdClearColorImage, PFN_vkCmdClearColorImage, "vkCmdClearColorImage");
    LOAD_DEVICE(cmdPipelineBarrier, PFN_vkCmdPipelineBarrier, "vkCmdPipelineBarrier");
    LOAD_DEVICE(cmdPipelineBarrier2, PFN_vkCmdPipelineBarrier2, "vkCmdPipelineBarrier2");
    LOAD_DEVICE(cmdPipelineBarrier2KHR, PFN_vkCmdPipelineBarrier2KHR, "vkCmdPipelineBarrier2KHR");
    LOAD_DEVICE(cmdBeginRendering, PFN_vkCmdBeginRendering, "vkCmdBeginRendering");
    LOAD_DEVICE(cmdBeginRenderingKHR, PFN_vkCmdBeginRenderingKHR, "vkCmdBeginRenderingKHR");
    LOAD_DEVICE(cmdEndRendering, PFN_vkCmdEndRendering, "vkCmdEndRendering");
    LOAD_DEVICE(cmdEndRenderingKHR, PFN_vkCmdEndRenderingKHR, "vkCmdEndRenderingKHR");
    LOAD_DEVICE(cmdResetQueryPool, PFN_vkCmdResetQueryPool, "vkCmdResetQueryPool");
    LOAD_DEVICE(cmdWriteTimestamp, PFN_vkCmdWriteTimestamp, "vkCmdWriteTimestamp");
    LOAD_DEVICE(cmdWriteTimestamp2, PFN_vkCmdWriteTimestamp2, "vkCmdWriteTimestamp2");
    LOAD_DEVICE(cmdWriteTimestamp2KHR, PFN_vkCmdWriteTimestamp2KHR, "vkCmdWriteTimestamp2KHR");
    LOAD_DEVICE(createSampler, PFN_vkCreateSampler, "vkCreateSampler");
    LOAD_DEVICE(destroySampler, PFN_vkDestroySampler, "vkDestroySampler");
    LOAD_DEVICE(createBuffer, PFN_vkCreateBuffer, "vkCreateBuffer");
    LOAD_DEVICE(destroyBuffer, PFN_vkDestroyBuffer, "vkDestroyBuffer");
    LOAD_DEVICE(getBufferMemoryRequirements, PFN_vkGetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    LOAD_DEVICE(getImageMemoryRequirements, PFN_vkGetImageMemoryRequirements, "vkGetImageMemoryRequirements");
    LOAD_DEVICE(allocateMemory, PFN_vkAllocateMemory, "vkAllocateMemory");
    LOAD_DEVICE(freeMemory, PFN_vkFreeMemory, "vkFreeMemory");
    LOAD_DEVICE(bindBufferMemory, PFN_vkBindBufferMemory, "vkBindBufferMemory");
    LOAD_DEVICE(bindImageMemory, PFN_vkBindImageMemory, "vkBindImageMemory");
    LOAD_DEVICE(mapMemory, PFN_vkMapMemory, "vkMapMemory");
    LOAD_DEVICE(unmapMemory, PFN_vkUnmapMemory, "vkUnmapMemory");
    LOAD_DEVICE(flushMappedMemoryRanges, PFN_vkFlushMappedMemoryRanges, "vkFlushMappedMemoryRanges");
    LOAD_DEVICE(cmdCopyBufferToImage, PFN_vkCmdCopyBufferToImage, "vkCmdCopyBufferToImage");
#undef LOAD_DEVICE
    return d;
}

const DeviceDispatch* FindDevice(const std::shared_ptr<const TrackingSnapshot>& snapshot, VkDevice device) {
    auto it = snapshot->devices.find(device);
    return it == snapshot->devices.end() ? nullptr : &it->second;
}

const DeviceDispatch* FindNativeDevice(
    const std::shared_ptr<const TrackingSnapshot>& snapshot, VkDevice device) {
    auto it = snapshot->nativeDevices.find(device);
    return it == snapshot->nativeDevices.end() ? nullptr : &it->second;
}

VkDevice FindCommandBufferDevice(const std::shared_ptr<const TrackingSnapshot>& snapshot, VkCommandBuffer commandBuffer) {
    auto it = snapshot->commandBuffers.find(commandBuffer);
    return it == snapshot->commandBuffers.end() ? VK_NULL_HANDLE : it->second;
}

uint32_t FindCommandBufferQueueFamily(
    const std::shared_ptr<const TrackingSnapshot>& snapshot,
    VkCommandBuffer commandBuffer) {
    auto it = snapshot->commandBufferFamilies.find(commandBuffer);
    return it == snapshot->commandBufferFamilies.end()
               ? VK_QUEUE_FAMILY_IGNORED
               : it->second;
}

VKAPI_ATTR VkResult VKAPI_CALL hkCreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
VKAPI_ATTR void VKAPI_CALL hkDestroyInstance(VkInstance, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL hkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
VKAPI_ATTR void VKAPI_CALL hkDestroyDevice(VkDevice, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL hkCreateWin32SurfaceKHR(VkInstance, const VkWin32SurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
VKAPI_ATTR void VKAPI_CALL hkDestroySurfaceKHR(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hkGetInstanceProcAddr(VkInstance, const char*);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hkGetDeviceProcAddr(VkDevice, const char*);
VKAPI_ATTR void VKAPI_CALL hkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue*);
VKAPI_ATTR void VKAPI_CALL hkGetDeviceQueue2(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
VKAPI_ATTR VkResult VKAPI_CALL hkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
VKAPI_ATTR void VKAPI_CALL hkDestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL hkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
VKAPI_ATTR VkResult VKAPI_CALL hkCreateImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage*);
VKAPI_ATTR void VKAPI_CALL hkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL hkCreateImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView*);
VKAPI_ATTR void VKAPI_CALL hkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL hkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool*);
VKAPI_ATTR void VKAPI_CALL hkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL hkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer*);
VKAPI_ATTR void VKAPI_CALL hkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer*);
VKAPI_ATTR VkResult VKAPI_CALL hkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL hkQueueSubmit2(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL hkQueueSubmit2KHR(VkQueue, uint32_t, const VkSubmitInfo2*, VkFence);
VKAPI_ATTR VkResult VKAPI_CALL hkQueuePresentKHR(VkQueue, const VkPresentInfoKHR*);
VKAPI_ATTR void VKAPI_CALL hkCmdBlitImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t,
                                          const VkImageBlit*, VkFilter);
VKAPI_ATTR VkResult VKAPI_CALL hkCreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence*);
VKAPI_ATTR void VKAPI_CALL hkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks*);
VKAPI_ATTR VkResult VKAPI_CALL hkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore*);
VKAPI_ATTR void VKAPI_CALL hkDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*);

PFN_vkVoidFunction Substitute(const char* name, PFN_vkVoidFunction real) {
    if (!name || !real) { return real; }
#define SUBSTITUTE(vkName, hookName, storage) \
    if (strcmp(name, #vkName) == 0) {          \
        auto expected = decltype((storage).load()){}; \
        (storage).compare_exchange_strong(expected, reinterpret_cast<decltype(expected)>(real), std::memory_order_acq_rel); \
        return reinterpret_cast<PFN_vkVoidFunction>(&hookName); \
    }
    SUBSTITUTE(vkGetInstanceProcAddr, hkGetInstanceProcAddr, g_realGetInstanceProcAddr)
    SUBSTITUTE(vkGetDeviceProcAddr, hkGetDeviceProcAddr, g_realGetDeviceProcAddr)
    SUBSTITUTE(vkCreateInstance, hkCreateInstance, g_realCreateInstance)
    SUBSTITUTE(vkDestroyInstance, hkDestroyInstance, g_realDestroyInstance)
    SUBSTITUTE(vkCreateDevice, hkCreateDevice, g_realCreateDevice)
    SUBSTITUTE(vkDestroyDevice, hkDestroyDevice, g_realDestroyDevice)
    SUBSTITUTE(vkCreateWin32SurfaceKHR, hkCreateWin32SurfaceKHR, g_realCreateWin32SurfaceKHR)
    SUBSTITUTE(vkDestroySurfaceKHR, hkDestroySurfaceKHR, g_realDestroySurfaceKHR)
#undef SUBSTITUTE
#define DEVICE_SUBSTITUTE(vkName, hookName) \
    if (strcmp(name, #vkName) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&hookName)
    DEVICE_SUBSTITUTE(vkGetDeviceQueue, hkGetDeviceQueue);
    DEVICE_SUBSTITUTE(vkGetDeviceQueue2, hkGetDeviceQueue2);
    DEVICE_SUBSTITUTE(vkCreateSwapchainKHR, hkCreateSwapchainKHR);
    DEVICE_SUBSTITUTE(vkDestroySwapchainKHR, hkDestroySwapchainKHR);
    DEVICE_SUBSTITUTE(vkGetSwapchainImagesKHR, hkGetSwapchainImagesKHR);
    DEVICE_SUBSTITUTE(vkCreateImage, hkCreateImage);
    DEVICE_SUBSTITUTE(vkDestroyImage, hkDestroyImage);
    DEVICE_SUBSTITUTE(vkCreateImageView, hkCreateImageView);
    DEVICE_SUBSTITUTE(vkDestroyImageView, hkDestroyImageView);
    DEVICE_SUBSTITUTE(vkCreateCommandPool, hkCreateCommandPool);
    DEVICE_SUBSTITUTE(vkDestroyCommandPool, hkDestroyCommandPool);
    DEVICE_SUBSTITUTE(vkAllocateCommandBuffers, hkAllocateCommandBuffers);
    DEVICE_SUBSTITUTE(vkFreeCommandBuffers, hkFreeCommandBuffers);
    DEVICE_SUBSTITUTE(vkQueueSubmit, hkQueueSubmit);
    DEVICE_SUBSTITUTE(vkQueueSubmit2, hkQueueSubmit2);
    DEVICE_SUBSTITUTE(vkQueueSubmit2KHR, hkQueueSubmit2KHR);
    DEVICE_SUBSTITUTE(vkQueuePresentKHR, hkQueuePresentKHR);
    DEVICE_SUBSTITUTE(vkCmdBlitImage, hkCmdBlitImage);
    DEVICE_SUBSTITUTE(vkCreateFence, hkCreateFence);
    DEVICE_SUBSTITUTE(vkDestroyFence, hkDestroyFence);
    DEVICE_SUBSTITUTE(vkCreateSemaphore, hkCreateSemaphore);
    DEVICE_SUBSTITUTE(vkDestroySemaphore, hkDestroySemaphore);
#undef DEVICE_SUBSTITUTE
    return real;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hkGetInstanceProcAddr(VkInstance instance, const char* name) {
    const auto gipa = g_realGetInstanceProcAddr.load(std::memory_order_acquire);
    PFN_vkVoidFunction real = gipa ? gipa(instance, name) : nullptr;
    return Substitute(name, real);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hkGetDeviceProcAddr(VkDevice device, const char* name) {
    PFN_vkGetDeviceProcAddr gdpa = g_realGetDeviceProcAddr.load(std::memory_order_acquire);
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    if (const DeviceDispatch* d = FindDevice(snapshot, device); d && d->getDeviceProcAddr) { gdpa = d->getDeviceProcAddr; }
    PFN_vkVoidFunction real = gdpa ? gdpa(device, name) : nullptr;
    return Substitute(name, real);
}

VKAPI_ATTR VkResult VKAPI_CALL hkCreateWin32SurfaceKHR(
    VkInstance instance, const VkWin32SurfaceCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) {
    const auto createSurface = g_realCreateWin32SurfaceKHR.load(std::memory_order_acquire);
    if (!createSurface) return VK_ERROR_INITIALIZATION_FAILED;
    const VkResult result = createSurface(instance, info, allocator, surface);
    if (result == VK_SUCCESS && info && surface && *surface) {
        HWND hwnd = info->hwnd;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (hwnd && IsWindow(hwnd) && pid == GetCurrentProcessId()) {
            UpdateSnapshot([&](TrackingSnapshot& s) {
                s.surfaces[*surface] = { instance, hwnd };
            });
            Log("[VULKAN] Tracked Win32 surface for its exact Minecraft window.");
        } else {
            Log("[VULKAN] Ignored a Win32 surface with a non-process or invalid HWND.");
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL hkDestroySurfaceKHR(
    VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks* allocator) {
    if (const auto destroySurface = g_realDestroySurfaceKHR.load(std::memory_order_acquire)) destroySurface(instance, surface, allocator);
    UpdateSnapshot([&](TrackingSnapshot& s) { s.surfaces.erase(surface); });
}

VKAPI_ATTR VkResult VKAPI_CALL hkCreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator,
                                                 VkInstance* instance) {
    const auto gipa = g_realGetInstanceProcAddr.load(std::memory_order_acquire);
    auto createInstance = g_realCreateInstance.load(std::memory_order_acquire);
    if (!createInstance && gipa) {
        const auto resolved = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
        PFN_vkCreateInstance expected = nullptr;
        g_realCreateInstance.compare_exchange_strong(expected, resolved, std::memory_order_acq_rel);
        createInstance = g_realCreateInstance.load(std::memory_order_acquire);
    }
    if (!createInstance) { return VK_ERROR_INITIALIZATION_FAILED; }
    VkResult result = createInstance(info, allocator, instance);
    if (result == VK_SUCCESS && instance && *instance) {
        UpdateSnapshot([&](TrackingSnapshot& s) { s.instance = *instance; });
        if (gipa) {
            g_realCreateDevice.store(reinterpret_cast<PFN_vkCreateDevice>(gipa(*instance, "vkCreateDevice")), std::memory_order_release);
            g_realDestroyInstance.store(reinterpret_cast<PFN_vkDestroyInstance>(gipa(*instance, "vkDestroyInstance")), std::memory_order_release);
            g_realGetDeviceProcAddr.store(reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(*instance, "vkGetDeviceProcAddr")), std::memory_order_release);
        }
        Log("[VULKAN] Tracked VkInstance.");
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL hkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
    if (const auto destroyInstance = g_realDestroyInstance.load(std::memory_order_acquire)) destroyInstance(instance, allocator);
    UpdateSnapshot([&](TrackingSnapshot& s) {
        if (s.instance == instance) s.instance = VK_NULL_HANDLE;
    });
}

VKAPI_ATTR VkResult VKAPI_CALL hkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* info,
                                               const VkAllocationCallbacks* allocator, VkDevice* device) {
    const auto createDevice = g_realCreateDevice.load(std::memory_order_acquire);
    const auto gipa = g_realGetInstanceProcAddr.load(std::memory_order_acquire);
    if (!createDevice) { return VK_ERROR_INITIALIZATION_FAILED; }

    VkDeviceCreateInfo augmentedInfo{};
    std::vector<const char*> augmentedExtensions;
    const VkDeviceCreateInfo* createInfo = info;
    bool enabledExternalHostMemory = false;
    if (info && gipa) {
        bool alreadyEnabled = false;
        for (uint32_t index = 0; index < info->enabledExtensionCount; ++index) {
            if (info->ppEnabledExtensionNames[index] &&
                strcmp(info->ppEnabledExtensionNames[index],
                       VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) == 0) {
                alreadyEnabled = true;
                break;
            }
        }
        auto snapshot = g_snapshot.load(std::memory_order_acquire);
        auto enumerate =
            reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                gipa(
                    snapshot ? snapshot->instance : VK_NULL_HANDLE,
                    "vkEnumerateDeviceExtensionProperties"));
        uint32_t extensionCount = 0;
        std::vector<VkExtensionProperties> supported;
        if (enumerate &&
            enumerate(physicalDevice, nullptr, &extensionCount, nullptr) ==
                VK_SUCCESS &&
            extensionCount > 0) {
            supported.resize(extensionCount);
            if (enumerate(
                    physicalDevice, nullptr, &extensionCount,
                    supported.data()) == VK_SUCCESS) {
                enabledExternalHostMemory = alreadyEnabled ||
                    std::any_of(
                        supported.begin(), supported.end(),
                        [](const VkExtensionProperties& extension) {
                            return strcmp(
                                extension.extensionName,
                                VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) ==
                                0;
                        });
            }
        }
        if (enabledExternalHostMemory && !alreadyEnabled) {
            augmentedInfo = *info;
            augmentedExtensions.assign(
                info->ppEnabledExtensionNames,
                info->ppEnabledExtensionNames + info->enabledExtensionCount);
            augmentedExtensions.push_back(
                VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
            augmentedInfo.enabledExtensionCount =
                static_cast<uint32_t>(augmentedExtensions.size());
            augmentedInfo.ppEnabledExtensionNames =
                augmentedExtensions.data();
            createInfo = &augmentedInfo;
        }
    }

    VkResult result =
        createDevice(physicalDevice, createInfo, allocator, device);
    if (result == VK_SUCCESS && device && *device) {
        PFN_vkGetDeviceProcAddr gdpa = g_realGetDeviceProcAddr.load(std::memory_order_acquire);
        if (!gdpa && gipa) {
            auto snapshot = g_snapshot.load(std::memory_order_acquire);
            gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                gipa(snapshot->instance, "vkGetDeviceProcAddr"));
        }
        DeviceDispatch dispatch = BuildDeviceDispatch(physicalDevice, *device, gdpa);
        PFN_vkGetDeviceProcAddr nativeGdpa = nullptr;
        if (HMODULE layer = GetModuleHandleW(L"Toolscreen.dll")) {
            using GetLowerGdpaFn =
                PFN_vkGetDeviceProcAddr(VKAPI_PTR*)(VkDevice);
            if (auto getLower = reinterpret_cast<GetLowerGdpaFn>(
                    GetProcAddress(
                        layer,
                        "ToolscreenVulkanLayerGetDeviceProcAddr"))) {
                nativeGdpa = getLower(*device);
            }
        }
        DeviceDispatch nativeDispatch = BuildDeviceDispatch(
            physicalDevice, *device, nativeGdpa ? nativeGdpa : gdpa);
        g_realDestroyDevice.store(LoadDevice<PFN_vkDestroyDevice>(gdpa, *device, "vkDestroyDevice"), std::memory_order_release);
        UpdateSnapshot([&](TrackingSnapshot& s) {
            s.devices[*device] = dispatch;
            s.nativeDevices[*device] = nativeDispatch;
        });
        Log("[VULKAN] Tracked VkPhysicalDevice and VkDevice dispatch; "
            "rendererLowerLayerDispatch=" +
            std::string(nativeGdpa ? "true" : "false") +
            ", externalHostMemory=" +
            std::string(enabledExternalHostMemory ? "enabled" : "unavailable") +
            ".");
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL hkDestroyDevice(VkDevice device, const VkAllocationCallbacks* allocator) {
    // Defer native renderer teardown to the lower layer when it owns this
    // device. OBS's untouched upper DestroyDevice path can then retire its
    // capture fences before composition images are destroyed. The lower
    // callback still runs before the actual driver device destruction.
    bool lowerLayerOwnsDevice = false;
    if (HMODULE layer = GetModuleHandleW(L"Toolscreen.dll")) {
        using GetLowerGdpaFn =
            PFN_vkGetDeviceProcAddr(VKAPI_PTR*)(VkDevice);
        if (auto getLower = reinterpret_cast<GetLowerGdpaFn>(
                GetProcAddress(
                    layer, "ToolscreenVulkanLayerGetDeviceProcAddr"))) {
            lowerLayerOwnsDevice = getLower(device) != nullptr;
        }
    }
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    if (!lowerLayerOwnsDevice) {
        if (d && d->deviceWaitIdle) d->deviceWaitIdle(device);
        VulkanRenderer::OnDeviceDestroyed(device);
    }
    PFN_vkDestroyDevice destroy = d ? LoadDevice<PFN_vkDestroyDevice>(d->getDeviceProcAddr, device, "vkDestroyDevice")
                                    : g_realDestroyDevice.load(std::memory_order_acquire);
    if (destroy) destroy(device, allocator);
    UpdateSnapshot([&](TrackingSnapshot& s) {
        s.devices.erase(device);
        s.nativeDevices.erase(device);
        std::erase_if(s.images, [&](const auto& item) { return item.second.device == device; });
        std::erase_if(s.swapchains, [&](const auto& item) { return item.second.device == device; });
        std::erase_if(s.queues, [&](const auto& item) { return item.second.device == device; });
        std::erase_if(s.commandPools, [&](const auto& item) { return item.second == device; });
        std::erase_if(s.commandPoolFamilies, [&](const auto& item) {
            auto poolIt = s.commandPools.find(item.first);
            return poolIt == s.commandPools.end() || poolIt->second == device;
        });
        std::erase_if(s.commandBuffers, [&](const auto& item) { return item.second == device; });
        std::erase_if(s.commandBufferFamilies, [&](const auto& item) {
            auto bufferIt = s.commandBuffers.find(item.first);
            return bufferIt == s.commandBuffers.end() || bufferIt->second == device;
        });
        std::erase_if(s.fences, [&](const auto& item) { return item.second == device; });
        std::erase_if(s.semaphores, [&](const auto& item) { return item.second == device; });
    });
}

VKAPI_ATTR void VKAPI_CALL hkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    if (d && d->getDeviceQueue) d->getDeviceQueue(device, family, index, queue);
    if (queue && *queue) {
        UpdateSnapshot([&](TrackingSnapshot& s) { s.queues[*queue] = { device, family, index }; });
    }
}

VKAPI_ATTR void VKAPI_CALL hkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* queue) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    if (d && d->getDeviceQueue2) d->getDeviceQueue2(device, info, queue);
    if (info && queue && *queue) {
        UpdateSnapshot([&](TrackingSnapshot& s) { s.queues[*queue] = { device, info->queueFamilyIndex, info->queueIndex }; });
    }
}

VKAPI_ATTR VkResult VKAPI_CALL hkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                                     const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    if (!d || !d->createSwapchainKHR) return VK_ERROR_INITIALIZATION_FAILED;
    VkSwapchainCreateInfoKHR adjustedInfo{};
    const VkSwapchainCreateInfoKHR* createInfo = info;
    if (info) {
        adjustedInfo = *info;
        auto getCapabilities = reinterpret_cast<
            PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
            LoadRealFunction(
                "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", nullptr));
        VkSurfaceCapabilitiesKHR capabilities{};
        if (getCapabilities &&
            getCapabilities(
                d->physicalDevice, info->surface, &capabilities) ==
                VK_SUCCESS) {
            if ((adjustedInfo.imageUsage &
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0 &&
                (capabilities.supportedUsageFlags &
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0) {
                adjustedInfo.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                Log("[VULKAN] Added supported color-attachment usage to Minecraft's swapchain.");
            }
            if ((adjustedInfo.imageUsage &
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 &&
                (capabilities.supportedUsageFlags &
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
                adjustedInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                Log("[VULKAN] Added supported transfer-source usage to "
                    "Minecraft's swapchain for asynchronous screenshots.");
            }
        }
        createInfo = &adjustedInfo;
    }
    VkResult result = d->createSwapchainKHR(device, createInfo, allocator, swapchain);
    if (result == VK_SUCCESS && info && swapchain && *swapchain) {
        SwapchainMetadata metadata{};
        metadata.device = device;
        metadata.swapchain = *swapchain;
        metadata.oldSwapchain = info->oldSwapchain;
        metadata.format = info->imageFormat;
        metadata.colorSpace = info->imageColorSpace;
        metadata.extent = info->imageExtent;
        metadata.usage = createInfo->imageUsage;
        metadata.minImageCount = info->minImageCount;
        if (const auto surface = snapshot->surfaces.find(info->surface);
            surface != snapshot->surfaces.end()) {
            metadata.hwnd = surface->second.hwnd;
        }
        UpdateSnapshot([&](TrackingSnapshot& s) { s.swapchains[*swapchain] = metadata; });
        Log("[VULKAN] Tracked swapchain " + std::to_string(info->imageExtent.width) + "x" +
            std::to_string(info->imageExtent.height) + ".");
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL hkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                  const VkAllocationCallbacks* allocator) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    if (const auto it = snapshot->swapchains.find(swapchain); it != snapshot->swapchains.end()) {
        VulkanRenderer::OnSwapchainDestroyed(device, swapchain, it->second.images);
    }
    if (d && d->destroySwapchainKHR) d->destroySwapchainKHR(device, swapchain, allocator);
    UpdateSnapshot([&](TrackingSnapshot& s) {
        s.swapchains.erase(swapchain);
        std::erase_if(s.images, [&](const auto& item) { return item.second.swapchain == swapchain; });
    });
}

VKAPI_ATTR VkResult VKAPI_CALL hkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* count,
                                                       VkImage* images) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    if (!d || !d->getSwapchainImagesKHR) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = d->getSwapchainImagesKHR(device, swapchain, count, images);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && count && images) {
        const uint32_t imageCount = *count;
        UpdateSnapshot([&](TrackingSnapshot& s) {
            auto swapIt = s.swapchains.find(swapchain);
            if (swapIt == s.swapchains.end()) return;
            swapIt->second.images.assign(images, images + imageCount);
            for (uint32_t i = 0; i < imageCount; ++i) {
                ImageMetadata metadata{};
                metadata.device = device;
                metadata.format = swapIt->second.format;
                metadata.extent = { swapIt->second.extent.width, swapIt->second.extent.height, 1 };
                metadata.usage = swapIt->second.usage;
                metadata.swapchainImage = true;
                metadata.swapchain = swapchain;
                s.images[images[i]] = metadata;
            }
        });
        Log("[VULKAN] Tracked " + std::to_string(imageCount) + " swapchain images.");
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hkCreateImage(VkDevice device, const VkImageCreateInfo* info,
                                              const VkAllocationCallbacks* allocator, VkImage* image) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    if (!d || !d->createImage) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = d->createImage(device, info, allocator, image);
    if (result == VK_SUCCESS && info && image && *image) {
        ImageMetadata metadata{};
        metadata.device = device;
        metadata.format = info->format;
        metadata.extent = info->extent;
        metadata.usage = info->usage;
        metadata.type = info->imageType;
        metadata.samples = info->samples;
        metadata.tiling = info->tiling;
        UpdateSnapshot([&](TrackingSnapshot& s) { s.images[*image] = metadata; });
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL hkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    VulkanRenderer::OnImageDestroyed(device, image);
    if (d && d->destroyImage) d->destroyImage(device, image, allocator);
    UpdateSnapshot([&](TrackingSnapshot& s) { s.images.erase(image); });
}

VKAPI_ATTR VkResult VKAPI_CALL hkCreateImageView(VkDevice device, const VkImageViewCreateInfo* info,
                                                 const VkAllocationCallbacks* allocator, VkImageView* view) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    if (!d || !d->createImageView) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = d->createImageView(device, info, allocator, view);
    if (result == VK_SUCCESS && info && view && *view) {
        UpdateSnapshot([&](TrackingSnapshot& s) { s.imageViews[*view] = info->image; });
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL hkDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks* allocator) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    if (d && d->destroyImageView) d->destroyImageView(device, view, allocator);
    UpdateSnapshot([&](TrackingSnapshot& s) { s.imageViews.erase(view); });
}

VKAPI_ATTR VkResult VKAPI_CALL hkCreateCommandPool(VkDevice device, const VkCommandPoolCreateInfo* info,
                                                    const VkAllocationCallbacks* allocator, VkCommandPool* pool) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    if (!d || !d->createCommandPool) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = d->createCommandPool(device, info, allocator, pool);
    if (result == VK_SUCCESS && pool && *pool) {
        UpdateSnapshot([&](TrackingSnapshot& s) {
            s.commandPools[*pool] = device;
            s.commandPoolFamilies[*pool] =
                info ? info->queueFamilyIndex : VK_QUEUE_FAMILY_IGNORED;
        });
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL hkDestroyCommandPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    if (d && d->destroyCommandPool) d->destroyCommandPool(device, pool, allocator);
    UpdateSnapshot([&](TrackingSnapshot& s) {
        s.commandPools.erase(pool);
        s.commandPoolFamilies.erase(pool);
        // Command buffers are implicitly freed. Their stale entries are harmless
        // until the handle is reused, when allocation overwrites the mapping.
    });
}

VKAPI_ATTR VkResult VKAPI_CALL hkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info,
                                                        VkCommandBuffer* buffers) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    if (!d || !d->allocateCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = d->allocateCommandBuffers(device, info, buffers);
    if (result == VK_SUCCESS && info && buffers) {
        UpdateSnapshot([&](TrackingSnapshot& s) {
            const auto familyIt = s.commandPoolFamilies.find(info->commandPool);
            const uint32_t family = familyIt == s.commandPoolFamilies.end()
                                        ? VK_QUEUE_FAMILY_IGNORED
                                        : familyIt->second;
            for (uint32_t i = 0; i < info->commandBufferCount; ++i) {
                s.commandBuffers[buffers[i]] = device;
                s.commandBufferFamilies[buffers[i]] = family;
            }
        });
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL hkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32_t count,
                                                 const VkCommandBuffer* buffers) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    const DeviceDispatch* d = FindDevice(snapshot, device);
    if (d && d->freeCommandBuffers) d->freeCommandBuffers(device, pool, count, buffers);
    if (buffers) {
        UpdateSnapshot([&](TrackingSnapshot& s) {
            for (uint32_t i = 0; i < count; ++i) {
                s.commandBuffers.erase(buffers[i]);
                s.commandBufferFamilies.erase(buffers[i]);
            }
        });
    }
}

template <typename SubmitInfo>
std::vector<VkCommandBuffer> CollectSubmittedCommandBuffers(uint32_t submitCount, const SubmitInfo* submits) {
    std::vector<VkCommandBuffer> result;
    if (!submits) return result;
    for (uint32_t i = 0; i < submitCount; ++i) {
        if constexpr (std::is_same_v<SubmitInfo, VkSubmitInfo>) {
            result.insert(result.end(), submits[i].pCommandBuffers,
                          submits[i].pCommandBuffers + submits[i].commandBufferCount);
        } else {
            for (uint32_t j = 0; j < submits[i].commandBufferInfoCount; ++j) {
                result.push_back(submits[i].pCommandBufferInfos[j].commandBuffer);
            }
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* submits, VkFence fence) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    auto queueIt = snapshot->queues.find(queue);
    if (queueIt == snapshot->queues.end()) return VK_ERROR_INITIALIZATION_FAILED;
    const DeviceDispatch* d = FindDevice(snapshot, queueIt->second.device);
    if (!d || !d->queueSubmit) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = d->queueSubmit(queue, submitCount, submits, fence);
    if (result == VK_SUCCESS) {
        auto buffers = CollectSubmittedCommandBuffers(submitCount, submits);
        VulkanRenderer::OnQueueSubmit(queueIt->second.device, queue, static_cast<uint32_t>(buffers.size()), buffers.data(), fence);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* submits, VkFence fence) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    auto queueIt = snapshot->queues.find(queue);
    if (queueIt == snapshot->queues.end()) return VK_ERROR_INITIALIZATION_FAILED;
    const DeviceDispatch* d = FindDevice(snapshot, queueIt->second.device);
    PFN_vkQueueSubmit2 submit = d ? d->queueSubmit2 : nullptr;
    if (!submit) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult result = submit(queue, submitCount, submits, fence);
    if (result == VK_SUCCESS) {
        auto buffers = CollectSubmittedCommandBuffers(submitCount, submits);
        VulkanRenderer::OnQueueSubmit(queueIt->second.device, queue, static_cast<uint32_t>(buffers.size()), buffers.data(), fence);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hkQueueSubmit2KHR(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* submits, VkFence fence) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    auto queueIt = snapshot->queues.find(queue);
    if (queueIt == snapshot->queues.end()) return VK_ERROR_INITIALIZATION_FAILED;
    const DeviceDispatch* d = FindDevice(snapshot, queueIt->second.device);
    PFN_vkQueueSubmit2KHR submit = d ? d->queueSubmit2KHR : nullptr;
    if (!submit) return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkResult result = submit(queue, submitCount, submits, fence);
    if (result == VK_SUCCESS) {
        auto buffers = CollectSubmittedCommandBuffers(submitCount, submits);
        VulkanRenderer::OnQueueSubmit(queueIt->second.device, queue, static_cast<uint32_t>(buffers.size()), buffers.data(), fence);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL hkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* presentInfo) {
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    auto queueIt = snapshot->queues.find(queue);
    if (queueIt == snapshot->queues.end()) return VK_ERROR_INITIALIZATION_FAILED;
    const DeviceDispatch* d = FindDevice(snapshot, queueIt->second.device);
    if (!d || !d->queuePresentKHR) return VK_ERROR_EXTENSION_NOT_PRESENT;

    // Call the renderer before forwarding to the next layer.  When OBS is
    // injected, the stored dispatch function remains its vkQueuePresentKHR
    // wrapper, so this observes the exact swapchain/image handoff without
    // bypassing OBS or altering its synchronization chain.
    std::vector<VkImage> presentImages;
    if (presentInfo && presentInfo->pSwapchains && presentInfo->pImageIndices) {
        presentImages.resize(presentInfo->swapchainCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < presentInfo->swapchainCount; ++i) {
            const auto swapchainIt = snapshot->swapchains.find(presentInfo->pSwapchains[i]);
            const uint32_t imageIndex = presentInfo->pImageIndices[i];
            if (swapchainIt != snapshot->swapchains.end() && imageIndex < swapchainIt->second.images.size()) {
                presentImages[i] = swapchainIt->second.images[imageIndex];
            }
        }
    }
    VulkanRenderer::OnQueuePresent(queueIt->second.device, queue, presentInfo, presentImages);
    return d->queuePresentKHR(queue, presentInfo);
}

VKAPI_ATTR void VKAPI_CALL hkCmdBlitImage(VkCommandBuffer commandBuffer, VkImage source, VkImageLayout sourceLayout,
                                          VkImage destination, VkImageLayout destinationLayout, uint32_t regionCount,
                                          const VkImageBlit* regions, VkFilter filter) {
    PROFILE_SCOPE_CAT("Vulkan present/final-blit hook", "Vulkan");
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    VkDevice device = FindCommandBufferDevice(snapshot, commandBuffer);
    const DeviceDispatch* dispatch = FindDevice(snapshot, device);
    const DeviceDispatch* nativeDispatch = FindNativeDevice(snapshot, device);
    if (!dispatch || !dispatch->cmdBlitImage) {
        // A command buffer can be created through a loader path we did not see
        // (for example, before the redirect layer was attached). Never drop a
        // recording call just because tracking is incomplete; the loader's
        // instance entry point is still a valid pass-through trampoline.
        if (const auto gipa = g_realGetInstanceProcAddr.load(std::memory_order_acquire); gipa && snapshot->instance) {
            auto fallback = reinterpret_cast<PFN_vkCmdBlitImage>(
                gipa(snapshot->instance, "vkCmdBlitImage"));
            if (fallback) {
                fallback(commandBuffer, source, sourceLayout, destination,
                         destinationLayout, regionCount, regions, filter);
            }
        }
        return;
    }
    if (!nativeDispatch || !nativeDispatch->cmdBlitImage)
        nativeDispatch = dispatch;

    auto dstIt = snapshot->images.find(destination);
    if (!g_loggedFirstBlit.exchange(true, std::memory_order_acq_rel)) {
        Log("[VULKAN] First vkCmdBlitImage observed: commandBufferTracked=" + std::string(device ? "true" : "false") +
            ", destinationTracked=" + std::string(dstIt != snapshot->images.end() ? "true" : "false") +
            ", destinationLayout=" + std::to_string(static_cast<int>(destinationLayout)) + ".");
    }
    // A tracked swapchain image is the authoritative proof that this is the
    // present path for the Minecraft device.  Do not make the first native
    // frame depend on command-line version parsing: Prism's instance metadata
    // is available before LWJGL's Vulkan work, while the process command line
    // is not guaranteed to retain the launcher version flag.  The old gate
    // therefore made an otherwise valid 26.2 final blit invisible to the
    // Vulkan renderer.
    const bool hasTrackedDestination = dstIt != snapshot->images.end() && dstIt->second.swapchainImage;
    const auto swapIt = hasTrackedDestination ? snapshot->swapchains.find(dstIt->second.swapchain)
                                              : snapshot->swapchains.end();
    const bool canonicalRegion =
        regionCount == 1 && regions &&
        regions[0].srcSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
        regions[0].dstSubresource.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
        regions[0].srcSubresource.mipLevel == 0 &&
        regions[0].dstSubresource.mipLevel == 0 &&
        regions[0].srcSubresource.baseArrayLayer == 0 &&
        regions[0].dstSubresource.baseArrayLayer == 0 &&
        regions[0].srcSubresource.layerCount == 1 &&
        regions[0].dstSubresource.layerCount == 1;
    const bool finalBlit = hasTrackedDestination && swapIt != snapshot->swapchains.end() &&
                           destinationLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                           canonicalRegion;
    if (!g_loggedFinalBlitDecision.exchange(true, std::memory_order_acq_rel)) {
        Log("[VULKAN] Final-blit classifier: versionEligible=" +
            std::string(IsMinecraft26_2FinalOrNewer(g_gameVersion) ? "true" : "false") +
            ", trackedDestination=" + std::string(hasTrackedDestination ? "true" : "false") +
            ", trackedSwapchain=" + std::string(swapIt != snapshot->swapchains.end() ? "true" : "false") +
            ", transferDestinationLayout=" +
            std::string(destinationLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL ? "true" : "false") +
            ", canonicalRegion=" + std::string(canonicalRegion ? "true" : "false") +
            ", accepted=" + std::string(finalBlit ? "true" : "false") + ".");
    }
    if (!finalBlit) {
        dispatch->cmdBlitImage(commandBuffer, source, sourceLayout, destination, destinationLayout, regionCount, regions, filter);
        return;
    }

    auto srcIt = snapshot->images.find(source);
    VulkanRenderer::FinalBlitContext context{};
    context.commandBuffer = commandBuffer;
    context.device = device;
    context.sourceImage = source;
    context.sourceLayout = sourceLayout;
    context.destinationImage = destination;
    context.destinationLayout = destinationLayout;
    context.commandBufferQueueFamily =
        FindCommandBufferQueueFamily(snapshot, commandBuffer);
    context.regionCount = regionCount;
    context.regions = regions;
    context.filter = filter;
    context.dispatch = nativeDispatch;
    context.sourceMetadata = srcIt == snapshot->images.end() ? nullptr : &srcIt->second;
    context.destinationMetadata = &dstIt->second;
    context.swapchain = swapIt == snapshot->swapchains.end() ? nullptr : &swapIt->second;
    if (!VulkanRenderer::RecordAfterFinalBlit(
            context, nativeDispatch->cmdBlitImage)) {
        dispatch->cmdBlitImage(commandBuffer, source, sourceLayout, destination, destinationLayout, regionCount, regions, filter);
    }
}

#define DEFINE_SYNC_CREATE(kind, Kind, field, mapField) \
VKAPI_ATTR VkResult VKAPI_CALL hkCreate##Kind(VkDevice device, const Vk##Kind##CreateInfo* info, \
                                               const VkAllocationCallbacks* allocator, Vk##Kind* value) { \
    auto snapshot = g_snapshot.load(std::memory_order_acquire); \
    const DeviceDispatch* d = FindDevice(snapshot, device); \
    if (!d || !d->field) return VK_ERROR_INITIALIZATION_FAILED; \
    VkResult result = d->field(device, info, allocator, value); \
    if (result == VK_SUCCESS && value && *value) UpdateSnapshot([&](TrackingSnapshot& s) { s.mapField[*value] = device; }); \
    return result; \
} \
VKAPI_ATTR void VKAPI_CALL hkDestroy##Kind(VkDevice device, Vk##Kind value, const VkAllocationCallbacks* allocator) { \
    auto snapshot = g_snapshot.load(std::memory_order_acquire); \
    const DeviceDispatch* d = FindDevice(snapshot, device); \
    if (d && d->destroy##Kind) d->destroy##Kind(device, value, allocator); \
    UpdateSnapshot([&](TrackingSnapshot& s) { s.mapField.erase(value); }); \
}
DEFINE_SYNC_CREATE(fence, Fence, createFence, fences)
DEFINE_SYNC_CREATE(semaphore, Semaphore, createSemaphore, semaphores)
#undef DEFINE_SYNC_CREATE

} // namespace

namespace VulkanHooks {

std::shared_ptr<const TrackingSnapshot> GetSnapshot() {
    return g_snapshot.load(std::memory_order_acquire);
}

bool InstallIfAvailable() {
    // LWJGL installation completes lazily when it asks Windows for
    // vkGetInstanceProcAddr. Avoid modifying exports in a DLL that has only
    // just returned from LoadLibrary.
    return g_installed.load(std::memory_order_acquire);
}

bool EnableProcessLocalObsRedirectLayer(HMODULE toolscreenModule) {
    if (!toolscreenModule) {
        LogCategory("init", "[VULKAN][LAYER] Process-local layer enable failed: Toolscreen module is null.");
        return false;
    }
    std::vector<wchar_t> modulePath(32768);
    const DWORD length = GetModuleFileNameW(
        toolscreenModule, modulePath.data(),
        static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) {
        LogCategory("init", "[VULKAN][LAYER] Process-local layer enable failed: module path unavailable.");
        return false;
    }
    std::wstring directory(modulePath.data(), length);
    const size_t slash = directory.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        LogCategory("init", "[VULKAN][LAYER] Process-local layer enable failed: module directory unavailable.");
        return false;
    }
    directory.resize(slash);
    const std::wstring layerDll = directory + L"\\Toolscreen.dll";
    const std::wstring manifest =
        directory + L"\\VK_LAYER_TOOLSCREEN_obs_redirect.json";
    if (GetFileAttributesW(layerDll.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(manifest.c_str()) == INVALID_FILE_ATTRIBUTES) {
        LogCategory(
            "init",
            "[VULKAN][LAYER] Process-local layer unavailable; expected " +
                WideToUtf8(layerDll) + " and " + WideToUtf8(manifest) +
                ". Stock Vulkan/OBS behavior will be preserved.");
        return false;
    }

    auto appendEnvironmentValue = [](const wchar_t* name,
                                     const std::wstring& value,
                                     wchar_t separator) {
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        std::wstring combined;
        if (required > 1) {
            combined.resize(required);
            const DWORD copied = GetEnvironmentVariableW(
                name, combined.data(), required);
            combined.resize(copied);
            const bool alreadyPresent =
                combined == value ||
                combined.find(std::wstring(1, separator) + value) !=
                    std::wstring::npos ||
                combined.find(value + std::wstring(1, separator)) !=
                    std::wstring::npos;
            if (!alreadyPresent) {
                combined.push_back(separator);
                combined += value;
            }
        } else {
            combined = value;
        }
        return SetEnvironmentVariableW(name, combined.c_str()) != FALSE;
    };
    auto obsLayerRegisteredInHive = [](HKEY hive) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(
                hive, L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", 0,
                KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
            return false;
        }
        bool found = false;
        for (DWORD index = 0; !found; ++index) {
            wchar_t name[32768]{};
            DWORD nameLength = static_cast<DWORD>(std::size(name));
            DWORD type = 0;
            DWORD enabled = 1;
            DWORD dataLength = sizeof(enabled);
            const LSTATUS status = RegEnumValueW(
                key, index, name, &nameLength, nullptr, &type,
                reinterpret_cast<BYTE*>(&enabled), &dataLength);
            if (status == ERROR_NO_MORE_ITEMS) break;
            if (status != ERROR_SUCCESS || type != REG_DWORD || enabled != 0)
                continue;
            std::wstring path(name, nameLength);
            const size_t slash = path.find_last_of(L"\\/");
            const std::wstring leaf =
                slash == std::wstring::npos ? path : path.substr(slash + 1);
            found =
                _wcsicmp(leaf.c_str(), L"obs-vulkan64.json") == 0 &&
                GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        }
        RegCloseKey(key);
        return found;
    };
    const bool obsLayerRegistered =
        obsLayerRegisteredInHive(HKEY_CURRENT_USER) ||
        obsLayerRegisteredInHive(HKEY_LOCAL_MACHINE);

    std::wstring orderedLayers =
        obsLayerRegistered
            ? L"VK_LAYER_OBS_HOOK;VK_LAYER_TOOLSCREEN_obs_redirect"
            : L"VK_LAYER_TOOLSCREEN_obs_redirect";
    const DWORD existingLength =
        GetEnvironmentVariableW(L"VK_INSTANCE_LAYERS", nullptr, 0);
    if (existingLength > 1) {
        std::wstring existing(existingLength, L'\0');
        existing.resize(GetEnvironmentVariableW(
            L"VK_INSTANCE_LAYERS", existing.data(), existingLength));
        size_t start = 0;
        while (start <= existing.size()) {
            const size_t end = existing.find(L';', start);
            const std::wstring layer = existing.substr(
                start, end == std::wstring::npos ? std::wstring::npos
                                                 : end - start);
            if (!layer.empty() &&
                _wcsicmp(layer.c_str(), L"VK_LAYER_OBS_HOOK") != 0 &&
                _wcsicmp(
                    layer.c_str(),
                    L"VK_LAYER_TOOLSCREEN_obs_redirect") != 0) {
                orderedLayers += L";" + layer;
            }
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
    }

    const bool pathSet =
        appendEnvironmentValue(L"VK_LAYER_PATH", directory, L';') &&
        appendEnvironmentValue(L"VK_ADD_LAYER_PATH", directory, L';');
    const bool layerSet = SetEnvironmentVariableW(
        L"VK_INSTANCE_LAYERS", orderedLayers.c_str()) != FALSE;
    if (!pathSet || !layerSet) {
        LogCategory(
            "init",
            "[VULKAN][LAYER] Failed to enable the process-local Vulkan layer (Win32 error " +
                std::to_string(GetLastError()) +
                "); stock Vulkan/OBS behavior will be preserved.");
        return false;
    }
    LogCategory(
        "init",
        "[VULKAN][LAYER] Process-local OBS redirect layer enabled before Vulkan instance/device creation: "
        "orderedLayers=" + WideToUtf8(orderedLayers) +
            " (first is top-most/closest to application), directory=" +
            WideToUtf8(directory) +
            ", obsLayerRegistered=" +
            std::string(obsLayerRegistered ? "true" : "false") +
            ". No registry or OBS installation state was modified.");
    return true;
}

void NotifyModuleLoaded(HMODULE module) {
    if (!module) return;
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return;
    std::wstring_view name(path);
    size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) name.remove_prefix(slash + 1);
    if (_wcsicmp(std::wstring(name).c_str(), L"vulkan-1.dll") == 0 ||
        _wcsicmp(std::wstring(name).c_str(), L"lwjgl.dll") == 0) {
        LogCategory("init", "[VULKAN] Observed graphics module " + WideToUtf8(std::wstring(name)) +
                                "; awaiting proc-address resolution.");
    }
}

FARPROC InterceptLoaderGetProcAddress(HMODULE module, LPCSTR name, FARPROC realFunction) {
    if (!module || !name || reinterpret_cast<uintptr_t>(name) <= 0xFFFFu || !realFunction) {
        return realFunction;
    }

    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return realFunction;
    std::wstring_view moduleName(path);
    const size_t slash = moduleName.find_last_of(L"\\/");
    if (slash != std::wstring_view::npos) moduleName.remove_prefix(slash + 1);
    if (_wcsicmp(std::wstring(moduleName).c_str(), L"vulkan-1.dll") != 0) {
        return realFunction;
    }

    PFN_vkVoidFunction real = reinterpret_cast<PFN_vkVoidFunction>(realFunction);
    if (strcmp(name, "vkGetInstanceProcAddr") == 0) {
        g_realGetInstanceProcAddr.store(reinterpret_cast<PFN_vkGetInstanceProcAddr>(realFunction), std::memory_order_release);
        g_installed.store(true, std::memory_order_release);
        Log("[VULKAN] LWJGL requested vkGetInstanceProcAddr; proc-address interception is active.");
    }
    return reinterpret_cast<FARPROC>(Substitute(name, real));
}

PFN_vkVoidFunction LoadRealFunction(const char* name, void* userData) {
    VkDevice device = reinterpret_cast<VkDevice>(userData);
    auto snapshot = g_snapshot.load(std::memory_order_acquire);
    if (device) {
        if (const DeviceDispatch* d = FindNativeDevice(snapshot, device);
            d && d->getDeviceProcAddr) {
            if (PFN_vkVoidFunction fn = d->getDeviceProcAddr(device, name)) return fn;
        }
    }
    if (const auto gipa = g_realGetInstanceProcAddr.load(std::memory_order_acquire)) return gipa(snapshot->instance, name);
    return nullptr;
}

void Shutdown() {
    VulkanRenderer::Shutdown();
    g_installed.store(false, std::memory_order_release);
}

} // namespace VulkanHooks
