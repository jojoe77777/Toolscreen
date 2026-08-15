#include <Windows.h>
#include <intrin.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "../vulkan_obs_redirect_api.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct InstanceState {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
};

struct ImageState {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    VkImageUsageFlags usage = 0;
    bool swapchain = false;
    bool external = false;
    bool obsExport = false;
    VkExternalMemoryHandleTypeFlags externalTypes = 0;
};

struct SwapchainState {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
};

struct RedirectReservation {
    uint32_t slot = UINT32_MAX;
    uint64_t serial = 0;
    VkImage source = VK_NULL_HANDLE;
    VkImage redirectedSource = VK_NULL_HANDLE;
    VkImage destination = VK_NULL_HANDLE;
};

struct DeviceState {
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
    PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR getSwapchainImages = nullptr;
    PFN_vkCreateImage createImage = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkQueueSubmit2 queueSubmit2 = nullptr;
    PFN_vkQueueSubmit2KHR queueSubmit2KHR = nullptr;
    PFN_vkWaitForFences waitForFences = nullptr;
    PFN_vkDestroyFence destroyFence = nullptr;
    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkResetCommandPool resetCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers freeCommandBuffers = nullptr;
    std::unordered_map<VkImage, ImageState> images;
    std::unordered_map<VkSwapchainKHR, SwapchainState> swapchains;
    std::unordered_map<VkCommandPool, std::unordered_set<VkCommandBuffer>> pools;
    std::unordered_map<VkCommandBuffer, VkCommandPool> commandPools;
    std::unordered_map<VkCommandBuffer, RedirectReservation> reservations;
    std::unordered_map<VkFence, std::vector<RedirectReservation>> fences;
    uint32_t obsExportImageCount = 0;
};

using GetCompositionFn = bool (*)(
    VkDevice, VkImage, VkFormat, VkExtent2D, ToolscreenVulkanObsComposition*);
using RedirectSubmittedFn = void (*)(uint32_t, uint64_t, VkFence);
using RedirectRetiredFn = void (*)(uint32_t, uint64_t, VkFence, VkResult);
using RedirectAbandonedFn = void (*)(uint32_t, uint64_t, const char*);
using CaptureAvailabilityFn = void (*)(
    VkDevice, bool, VkImage, VkFormat, VkExtent2D);
using BeforeDeviceDestroyFn = void (*)(VkDevice);
using LayerLogFn = void (*)(const char*);

std::mutex g_mutex;
std::unordered_map<void*, InstanceState> g_instances;
std::unordered_map<void*, DeviceState> g_devices;
std::atomic<uint64_t> g_redirectCount{0};
std::atomic<uint64_t> g_passThroughCount{0};
std::atomic<uint64_t> g_lastRejectLogMs{0};
std::atomic<uint64_t> g_lastAcceptLogMs{0};
std::atomic<uint64_t> g_lastSubmitLogMs{0};
std::atomic<bool> g_loggedLoad{false};

template <typename T>
void* DispatchKey(T object) {
    return object ? *reinterpret_cast<void**>(object) : nullptr;
}

std::string Handle(uint64_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << value;
    return stream.str();
}

std::string ModuleForAddress(const void* address) {
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || !VirtualQuery(address, &memory, sizeof(memory)) || !memory.AllocationBase) {
        return "<unknown>";
    }
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(static_cast<HMODULE>(memory.AllocationBase), path, MAX_PATH)) {
        return "<unknown>";
    }
    const wchar_t* name = wcsrchr(path, L'\\');
    const std::wstring leaf = name ? name + 1 : path;
    if (leaf.empty()) return "<unknown>";
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, leaf.c_str(), static_cast<int>(leaf.size()), nullptr, 0,
        nullptr, nullptr);
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, leaf.c_str(), static_cast<int>(leaf.size()),
        result.data(), bytes, nullptr, nullptr);
    return result;
}

template <typename T>
T ResolveToolscreen(const char* name) {
    HMODULE module = GetModuleHandleW(L"Toolscreen.dll");
    return module ? reinterpret_cast<T>(GetProcAddress(module, name)) : nullptr;
}

void Log(const std::string& message) {
    const std::string full = "[VULKAN][LAYER] " + message;
    if (auto logger = ResolveToolscreen<LayerLogFn>("ToolscreenVulkanLayerLog")) {
        logger(full.c_str());
    } else {
        OutputDebugStringA((full + "\n").c_str());
    }
}

bool ShouldLogReject() {
    const uint64_t now = GetTickCount64();
    uint64_t previous = g_lastRejectLogMs.load(std::memory_order_relaxed);
    while (previous == 0 || now - previous >= 2000) {
        if (g_lastRejectLogMs.compare_exchange_weak(
                previous, now, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool ShouldLogAccept() {
    const uint64_t now = GetTickCount64();
    uint64_t previous = g_lastAcceptLogMs.load(std::memory_order_relaxed);
    while (previous == 0 || now - previous >= 2000) {
        if (g_lastAcceptLogMs.compare_exchange_weak(
                previous, now, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool ShouldLogSubmit() {
    const uint64_t now = GetTickCount64();
    uint64_t previous = g_lastSubmitLogMs.load(std::memory_order_relaxed);
    while (previous == 0 || now - previous >= 2000) {
        if (g_lastSubmitLogMs.compare_exchange_weak(
                previous, now, std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void Abandon(const RedirectReservation& reservation, const char* reason) {
    if (auto fn = ResolveToolscreen<RedirectAbandonedFn>(
            "ToolscreenVulkanObsRedirectAbandoned")) {
        fn(reservation.slot, reservation.serial, reason);
    }
}

void NotifyCaptureAvailability(
    VkDevice device, bool available, VkImage image, VkFormat format,
    VkExtent2D extent) {
    if (auto fn = ResolveToolscreen<CaptureAvailabilityFn>(
            "ToolscreenVulkanObsCaptureAvailability")) {
        fn(device, available, image, format, extent);
    }
}

void RetireFence(DeviceState& state, VkFence fence, VkResult result) {
    auto found = state.fences.find(fence);
    if (found == state.fences.end()) return;
    if (auto fn = ResolveToolscreen<RedirectRetiredFn>(
            "ToolscreenVulkanObsRedirectRetired")) {
        for (const RedirectReservation& reservation : found->second) {
            fn(reservation.slot, reservation.serial, fence, result);
        }
    }
    state.fences.erase(found);
}

bool IsObsCaller(const void* caller) {
    return _stricmp(ModuleForAddress(caller).c_str(),
                    "graphics-hook64.dll") == 0;
}

bool FindExternalInfo(
    const void* chain, VkExternalMemoryHandleTypeFlags& types) {
    for (auto* base = static_cast<const VkBaseInStructure*>(chain); base;
         base = base->pNext) {
        if (base->sType ==
            VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) {
            const auto* external =
                reinterpret_cast<const VkExternalMemoryImageCreateInfo*>(base);
            types = external->handleTypes;
            return types != 0;
        }
    }
    return false;
}

DeviceState* FindDevice(VkDevice device) {
    auto found = g_devices.find(DispatchKey(device));
    return found == g_devices.end() ? nullptr : &found->second;
}

DeviceState* FindDeviceFromObject(void* object) {
    auto found = g_devices.find(object ? *reinterpret_cast<void**>(object) : nullptr);
    return found == g_devices.end() ? nullptr : &found->second;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetInstanceProcAddr(
    VkInstance instance, const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetDeviceProcAddr(
    VkDevice device, const char* name);
VKAPI_ATTR VkResult VKAPI_CALL LayerCreateInstance(
    const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator,
    VkInstance* instance);
VKAPI_ATTR void VKAPI_CALL LayerDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL LayerCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkDevice* device);
VKAPI_ATTR void VKAPI_CALL LayerDestroyDevice(
    VkDevice device, const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL LayerCreateSwapchain(
    VkDevice device, const VkSwapchainCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain);
VKAPI_ATTR void VKAPI_CALL LayerDestroySwapchain(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL LayerGetSwapchainImages(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t* count,
    VkImage* images);
VKAPI_ATTR VkResult VKAPI_CALL LayerCreateImage(
    VkDevice device, const VkImageCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkImage* image);
VKAPI_ATTR void VKAPI_CALL LayerDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks* allocator);
VKAPI_ATTR void VKAPI_CALL LayerCmdCopyImage(
    VkCommandBuffer commandBuffer, VkImage source, VkImageLayout sourceLayout,
    VkImage destination, VkImageLayout destinationLayout, uint32_t regionCount,
    const VkImageCopy* regions);
VKAPI_ATTR VkResult VKAPI_CALL LayerQueueSubmit(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo* submits,
    VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL LayerQueueSubmit2(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* submits,
    VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL LayerQueueSubmit2KHR(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* submits,
    VkFence fence);
VKAPI_ATTR VkResult VKAPI_CALL LayerWaitForFences(
    VkDevice device, uint32_t fenceCount, const VkFence* fences, VkBool32 waitAll,
    uint64_t timeout);
VKAPI_ATTR void VKAPI_CALL LayerDestroyFence(
    VkDevice device, VkFence fence, const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL LayerCreateCommandPool(
    VkDevice device, const VkCommandPoolCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkCommandPool* pool);
VKAPI_ATTR void VKAPI_CALL LayerDestroyCommandPool(
    VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator);
VKAPI_ATTR VkResult VKAPI_CALL LayerResetCommandPool(
    VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags);
VKAPI_ATTR VkResult VKAPI_CALL LayerAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* info,
    VkCommandBuffer* buffers);
VKAPI_ATTR void VKAPI_CALL LayerFreeCommandBuffers(
    VkDevice device, VkCommandPool pool, uint32_t count,
    const VkCommandBuffer* buffers);

PFN_vkVoidFunction DeviceOverride(const char* name) {
#define OVERRIDE(symbol) \
    if (strcmp(name, "vk" #symbol) == 0) \
        return reinterpret_cast<PFN_vkVoidFunction>(Layer##symbol)
    OVERRIDE(GetDeviceProcAddr);
    OVERRIDE(DestroyDevice);
    OVERRIDE(CreateImage);
    OVERRIDE(DestroyImage);
    OVERRIDE(CmdCopyImage);
    OVERRIDE(QueueSubmit);
    OVERRIDE(QueueSubmit2);
    OVERRIDE(QueueSubmit2KHR);
    OVERRIDE(WaitForFences);
    OVERRIDE(DestroyFence);
    OVERRIDE(CreateCommandPool);
    OVERRIDE(DestroyCommandPool);
    OVERRIDE(ResetCommandPool);
    OVERRIDE(AllocateCommandBuffers);
    OVERRIDE(FreeCommandBuffers);
#undef OVERRIDE
    if (strcmp(name, "vkCreateSwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(LayerCreateSwapchain);
    if (strcmp(name, "vkDestroySwapchainKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(LayerDestroySwapchain);
    if (strcmp(name, "vkGetSwapchainImagesKHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(LayerGetSwapchainImages);
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetInstanceProcAddr(
    VkInstance instance, const char* name) {
    if (!name) return nullptr;
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(LayerGetInstanceProcAddr);
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(LayerGetDeviceProcAddr);
    if (strcmp(name, "vkCreateInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(LayerCreateInstance);
    if (strcmp(name, "vkCreateDevice") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(LayerCreateDevice);
    if (strcmp(name, "vkDestroyInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(LayerDestroyInstance);
    std::lock_guard lock(g_mutex);
    auto found = g_instances.find(DispatchKey(instance));
    return found == g_instances.end() || !found->second.gipa
               ? nullptr
               : found->second.gipa(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL LayerGetDeviceProcAddr(
    VkDevice device, const char* name) {
    if (!name) return nullptr;
    if (PFN_vkVoidFunction override = DeviceOverride(name)) return override;
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    return state && state->gdpa ? state->gdpa(device, name) : nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateInstance(
    const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator,
    VkInstance* instance) {
    auto* link = const_cast<VkLayerInstanceCreateInfo*>(
        static_cast<const VkLayerInstanceCreateInfo*>(info->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO)) {
        link = const_cast<VkLayerInstanceCreateInfo*>(
            static_cast<const VkLayerInstanceCreateInfo*>(link->pNext));
    }
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr nextGipa =
        link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto nextCreate = reinterpret_cast<PFN_vkCreateInstance>(
        nextGipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = nextCreate(info, allocator, instance);
    if (result == VK_SUCCESS) {
        std::lock_guard lock(g_mutex);
        g_instances[DispatchKey(*instance)] =
            InstanceState{*instance, nextGipa};
        Log("loaded and enabled; caller=" + ModuleForAddress(_ReturnAddress()) +
            ", nextGIPA=" + ModuleForAddress(reinterpret_cast<void*>(nextGipa)) +
            ", nextCreateInstance=" +
            ModuleForAddress(reinterpret_cast<void*>(nextCreate)) +
            ". The layer is below this exact caller; OBS-above ordering is "
            "proven only when a graphics-hook64 export-copy reaches it.");
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL LayerDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks* allocator) {
    PFN_vkDestroyInstance destroy = nullptr;
    {
        std::lock_guard lock(g_mutex);
        auto found = g_instances.find(DispatchKey(instance));
        if (found != g_instances.end()) {
            destroy = reinterpret_cast<PFN_vkDestroyInstance>(
                found->second.gipa(instance, "vkDestroyInstance"));
            g_instances.erase(found);
        }
    }
    if (destroy) destroy(instance, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkDevice* device) {
    auto* link = const_cast<VkLayerDeviceCreateInfo*>(
        static_cast<const VkLayerDeviceCreateInfo*>(info->pNext));
    while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
                     link->function == VK_LAYER_LINK_INFO)) {
        link = const_cast<VkLayerDeviceCreateInfo*>(
            static_cast<const VkLayerDeviceCreateInfo*>(link->pNext));
    }
    if (!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr nextGipa =
        link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr nextGdpa =
        link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto nextCreate = reinterpret_cast<PFN_vkCreateDevice>(
        nextGipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!nextCreate) {
        nextCreate = reinterpret_cast<PFN_vkCreateDevice>(
            nextGipa(reinterpret_cast<VkInstance>(physicalDevice),
                     "vkCreateDevice"));
    }
    if (!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = nextCreate(physicalDevice, info, allocator, device);
    if (result != VK_SUCCESS) return result;

    DeviceState state{};
    state.device = *device;
    state.gdpa = nextGdpa;
#define LOAD(field, type, name) \
    state.field = reinterpret_cast<type>(nextGdpa(*device, name))
    LOAD(destroyDevice, PFN_vkDestroyDevice, "vkDestroyDevice");
    LOAD(deviceWaitIdle, PFN_vkDeviceWaitIdle, "vkDeviceWaitIdle");
    LOAD(createSwapchain, PFN_vkCreateSwapchainKHR, "vkCreateSwapchainKHR");
    LOAD(destroySwapchain, PFN_vkDestroySwapchainKHR, "vkDestroySwapchainKHR");
    LOAD(getSwapchainImages, PFN_vkGetSwapchainImagesKHR,
         "vkGetSwapchainImagesKHR");
    LOAD(createImage, PFN_vkCreateImage, "vkCreateImage");
    LOAD(destroyImage, PFN_vkDestroyImage, "vkDestroyImage");
    LOAD(cmdCopyImage, PFN_vkCmdCopyImage, "vkCmdCopyImage");
    LOAD(queueSubmit, PFN_vkQueueSubmit, "vkQueueSubmit");
    LOAD(queueSubmit2, PFN_vkQueueSubmit2, "vkQueueSubmit2");
    LOAD(queueSubmit2KHR, PFN_vkQueueSubmit2KHR, "vkQueueSubmit2KHR");
    LOAD(waitForFences, PFN_vkWaitForFences, "vkWaitForFences");
    LOAD(destroyFence, PFN_vkDestroyFence, "vkDestroyFence");
    LOAD(createCommandPool, PFN_vkCreateCommandPool, "vkCreateCommandPool");
    LOAD(destroyCommandPool, PFN_vkDestroyCommandPool, "vkDestroyCommandPool");
    LOAD(resetCommandPool, PFN_vkResetCommandPool, "vkResetCommandPool");
    LOAD(allocateCommandBuffers, PFN_vkAllocateCommandBuffers,
         "vkAllocateCommandBuffers");
    LOAD(freeCommandBuffers, PFN_vkFreeCommandBuffers, "vkFreeCommandBuffers");
#undef LOAD
    {
        std::lock_guard lock(g_mutex);
        g_devices[DispatchKey(*device)] = std::move(state);
    }
    Log("device chain active; caller=" + ModuleForAddress(_ReturnAddress()) +
        ", nextGDPA=" + ModuleForAddress(reinterpret_cast<void*>(nextGdpa)) +
        ", OBS hook detected=" +
        std::string(GetModuleHandleW(L"graphics-hook64.dll") ? "true" : "false") +
        ".");
    return result;
}

VKAPI_ATTR void VKAPI_CALL LayerDestroyDevice(
    VkDevice device, const VkAllocationCallbacks* allocator) {
    DeviceState state{};
    {
        std::lock_guard lock(g_mutex);
        auto found = g_devices.find(DispatchKey(device));
        if (found == g_devices.end()) return;
        state = std::move(found->second);
        g_devices.erase(found);
    }
    for (const auto& [buffer, reservation] : state.reservations)
        Abandon(reservation, "device destruction");
    for (const auto& [fence, reservations] : state.fences)
        for (const RedirectReservation& reservation : reservations)
            Abandon(reservation, "device destruction with capture in flight");
    if (state.obsExportImageCount)
        NotifyCaptureAvailability(
            device, false, VK_NULL_HANDLE, VK_FORMAT_UNDEFINED, {});
    // The redirect callback destroys Toolscreen-owned Vulkan objects. Ensure
    // command buffers that may reference those objects have completed before
    // handing the device to the renderer teardown path.
    if (state.deviceWaitIdle) {
        VkResult idleResult = state.deviceWaitIdle(device);
        if (idleResult != VK_SUCCESS) {
            Log("device wait idle before Toolscreen teardown returned " +
                std::to_string(static_cast<int>(idleResult)) + ".");
        }
    }
    if (auto beforeDestroy = ResolveToolscreen<BeforeDeviceDestroyFn>(
            "ToolscreenVulkanBeforeLowerDeviceDestroy")) {
        beforeDestroy(device);
    }
    Log("device destroyed; redirects=" +
        std::to_string(g_redirectCount.load()) + ", passThrough=" +
        std::to_string(g_passThroughCount.load()) + ".");
    if (state.destroyDevice) state.destroyDevice(device, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateSwapchain(
    VkDevice device, const VkSwapchainCreateInfoKHR* info,
    const VkAllocationCallbacks* allocator, VkSwapchainKHR* swapchain) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    if (!state || !state->createSwapchain) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = state->createSwapchain(device, info, allocator, swapchain);
    if (result == VK_SUCCESS) {
        state->swapchains[*swapchain] =
            SwapchainState{info->imageFormat, info->imageExtent, {}};
        Log("swapchain tracked=" +
            Handle(reinterpret_cast<uint64_t>(*swapchain)) + " extent=" +
            std::to_string(info->imageExtent.width) + "x" +
            std::to_string(info->imageExtent.height) + " format=" +
            std::to_string(static_cast<int>(info->imageFormat)) + ".");
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL LayerDestroySwapchain(
    VkDevice device, VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* allocator) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    if (!state || !state->destroySwapchain) return;
    auto found = state->swapchains.find(swapchain);
    if (found != state->swapchains.end()) {
        for (VkImage image : found->second.images) state->images.erase(image);
        state->swapchains.erase(found);
    }
    state->destroySwapchain(device, swapchain, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL LayerGetSwapchainImages(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t* count,
    VkImage* images) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    if (!state || !state->getSwapchainImages)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result =
        state->getSwapchainImages(device, swapchain, count, images);
    auto found = state->swapchains.find(swapchain);
    if ((result == VK_SUCCESS || result == VK_INCOMPLETE) && found != state->swapchains.end() &&
        count && images) {
        found->second.images.assign(images, images + *count);
        for (uint32_t index = 0; index < *count; ++index) {
            state->images[images[index]] = ImageState{
                found->second.format,
                {found->second.extent.width, found->second.extent.height, 1},
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT, true, false, false, 0};
        }
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateImage(
    VkDevice device, const VkImageCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkImage* image) {
    const void* caller = _ReturnAddress();
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    if (!state || !state->createImage) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = state->createImage(device, info, allocator, image);
    if (result == VK_SUCCESS) {
        VkExternalMemoryHandleTypeFlags types = 0;
        const bool external = FindExternalInfo(info->pNext, types);
        const bool obsExport =
            external && IsObsCaller(caller) &&
            (types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT) != 0 &&
            (info->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0 &&
            (info->usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0;
        state->images[*image] = ImageState{
            info->format, info->extent, info->usage, false, external,
            obsExport, types};
        if (external) {
            Log("external export-image candidate created=" +
                Handle(reinterpret_cast<uint64_t>(*image)) + " extent=" +
                std::to_string(info->extent.width) + "x" +
                std::to_string(info->extent.height) + " format=" +
                std::to_string(static_cast<int>(info->format)) + " handleTypes=" +
                Handle(types) + " caller=" + ModuleForAddress(caller) +
                " obsExport=" + (obsExport ? "true" : "false") + ".");
        }
        if (obsExport) {
            ++state->obsExportImageCount;
            NotifyCaptureAvailability(
                device, true, *image, info->format,
                {info->extent.width, info->extent.height});
            Log("OBS hook detected with active export image; capture "
                "composition is now enabled.");
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL LayerDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks* allocator) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    if (!state || !state->destroyImage) return;
    auto found = state->images.find(image);
    if (found != state->images.end() && found->second.obsExport) {
        if (state->obsExportImageCount) --state->obsExportImageCount;
        if (!state->obsExportImageCount) {
            NotifyCaptureAvailability(
                device, false, image, found->second.format,
                {found->second.extent.width, found->second.extent.height});
            Log("last OBS export image destroyed; capture composition disabled "
                "and stock pass-through retained.");
        }
    }
    state->images.erase(image);
    state->destroyImage(device, image, allocator);
}

VKAPI_ATTR void VKAPI_CALL LayerCmdCopyImage(
    VkCommandBuffer commandBuffer, VkImage source, VkImageLayout sourceLayout,
    VkImage destination, VkImageLayout destinationLayout, uint32_t regionCount,
    const VkImageCopy* regions) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDeviceFromObject(commandBuffer);
    if (!state || !state->cmdCopyImage) return;
    const auto sourceIt = state->images.find(source);
    const auto destinationIt = state->images.find(destination);
    std::string reject;
    const void* caller = _ReturnAddress();
    const bool obsCaller = IsObsCaller(caller);
    if (!GetModuleHandleW(L"graphics-hook64.dll"))
        reject = "OBS graphics-hook64.dll is absent";
    else if (!obsCaller)
        reject = "call origin is " + ModuleForAddress(caller) +
                 ", not graphics-hook64.dll";
    else if (sourceIt == state->images.end() || !sourceIt->second.swapchain)
        reject = "source is not a tracked Minecraft swapchain image";
    else if (destinationIt == state->images.end() ||
             !destinationIt->second.obsExport)
        reject = "destination is not a graphics-hook64 D3D11 KMT export image";
    else if (sourceLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        reject = "source layout is not TRANSFER_SRC_OPTIMAL";
    else if (destinationLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        reject = "destination layout is not TRANSFER_DST_OPTIMAL";
    else if (regionCount != 1 || !regions)
        reject = "copy is not exactly one region";
    else if (sourceIt->second.format != destinationIt->second.format)
        reject = "source/destination formats differ";
    else {
        const VkImageCopy& region = regions[0];
        const VkExtent3D expected = sourceIt->second.extent;
        if (region.srcOffset.x || region.srcOffset.y || region.srcOffset.z ||
            region.dstOffset.x || region.dstOffset.y || region.dstOffset.z ||
            region.extent.width != expected.width ||
            region.extent.height != expected.height || region.extent.depth != 1 ||
            region.srcSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
            region.dstSubresource.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT ||
            region.srcSubresource.mipLevel != 0 ||
            region.dstSubresource.mipLevel != 0 ||
            region.srcSubresource.baseArrayLayer != 0 ||
            region.dstSubresource.baseArrayLayer != 0 ||
            region.srcSubresource.layerCount != 1 ||
            region.dstSubresource.layerCount != 1) {
            reject = "copy is not OBS's full-frame color subresource";
        }
    }

    ToolscreenVulkanObsComposition composition{};
    if (reject.empty()) {
        auto get = ResolveToolscreen<GetCompositionFn>(
            "ToolscreenVulkanGetObsComposition");
        if (!get)
            reject = "Toolscreen composition API is unavailable";
        else if (!get(
                     state->device, source, sourceIt->second.format,
                     {sourceIt->second.extent.width,
                      sourceIt->second.extent.height},
                     &composition))
            reject = "no safe completed composition ring slot is available";
        else if (!composition.image ||
                 composition.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
            reject = "published composition has an invalid image/layout";
        else if (composition.format != sourceIt->second.format ||
                 composition.extent.width != sourceIt->second.extent.width ||
                 composition.extent.height != sourceIt->second.extent.height)
            reject = "published composition format/extent mismatch";
    }

    if (reject.empty()) {
        RedirectReservation reservation{
            composition.slot, composition.serial, source, composition.image,
            destination};
        auto old = state->reservations.find(commandBuffer);
        if (old != state->reservations.end()) {
            Abandon(old->second, "command buffer recorded another redirect");
        }
        state->reservations[commandBuffer] = reservation;
        state->cmdCopyImage(
            commandBuffer, composition.image, composition.layout, destination,
            destinationLayout, regionCount, regions);
        const uint64_t count = g_redirectCount.fetch_add(1) + 1;
        if (ShouldLogAccept()) {
            Log("OBS export-copy candidate ACCEPTED: sourceSwapchain=" +
                Handle(reinterpret_cast<uint64_t>(source)) +
                " redirectedComposition=" +
                Handle(reinterpret_cast<uint64_t>(composition.image)) +
                " destinationExport=" +
                Handle(reinterpret_cast<uint64_t>(destination)) + " extent=" +
                std::to_string(composition.extent.width) + "x" +
                std::to_string(composition.extent.height) + " format=" +
                std::to_string(static_cast<int>(composition.format)) +
                " layouts=" + std::to_string(static_cast<int>(sourceLayout)) +
                "->" + std::to_string(static_cast<int>(destinationLayout)) +
                " frame=" + std::to_string(composition.frameSerial) + " slot=" +
                std::to_string(composition.slot) + " serial=" +
                std::to_string(composition.serial) + " redirectCount=" +
                std::to_string(count) + ".");
        }
        return;
    }

    state->cmdCopyImage(commandBuffer, source, sourceLayout, destination,
                        destinationLayout, regionCount, regions);
    const uint64_t count = g_passThroughCount.fetch_add(1) + 1;
    if (ShouldLogReject()) {
        Log("copy candidate REJECTED/pass-through: reason=" + reject +
            ", source=" + Handle(reinterpret_cast<uint64_t>(source)) +
            ", destination=" +
            Handle(reinterpret_cast<uint64_t>(destination)) + ", extent=" +
            (regionCount && regions
                 ? std::to_string(regions[0].extent.width) + "x" +
                       std::to_string(regions[0].extent.height)
                 : "<none>") +
            ", layouts=" + std::to_string(static_cast<int>(sourceLayout)) +
            "->" + std::to_string(static_cast<int>(destinationLayout)) +
            ", passThroughCount=" + std::to_string(count) + ".");
    }
}

void CompleteRedirectReservations(
    DeviceState& state, std::vector<RedirectReservation>& reservations,
    VkFence fence, VkResult result) {
    if (result == VK_SUCCESS && fence && !reservations.empty()) {
        auto& associated = state.fences[fence];
        associated.insert(
            associated.end(), reservations.begin(), reservations.end());
        if (auto fn = ResolveToolscreen<RedirectSubmittedFn>(
                "ToolscreenVulkanObsRedirectSubmitted")) {
            for (const RedirectReservation& reservation : reservations)
                fn(reservation.slot, reservation.serial, fence);
        }
        if (ShouldLogSubmit()) {
            Log("redirected OBS command buffer submitted before real present; fence=" +
                Handle(reinterpret_cast<uint64_t>(fence)) + " reservations=" +
                std::to_string(reservations.size()) + ", redirectCount=" +
                std::to_string(g_redirectCount.load()) + ".");
        }
    } else {
        for (const RedirectReservation& reservation : reservations)
            Abandon(reservation, result == VK_SUCCESS
                                     ? "OBS submission had no fence"
                                     : "OBS queue submission failed");
    }
}

VKAPI_ATTR VkResult VKAPI_CALL LayerQueueSubmit(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo* submits,
    VkFence fence) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDeviceFromObject(queue);
    if (!state || !state->queueSubmit) return VK_ERROR_INITIALIZATION_FAILED;
    std::vector<RedirectReservation> reservations;
    for (uint32_t submitIndex = 0; submitIndex < submitCount; ++submitIndex) {
        for (uint32_t bufferIndex = 0;
             bufferIndex < submits[submitIndex].commandBufferCount;
             ++bufferIndex) {
            VkCommandBuffer buffer =
                submits[submitIndex].pCommandBuffers[bufferIndex];
            auto found = state->reservations.find(buffer);
            if (found != state->reservations.end()) {
                reservations.push_back(found->second);
                state->reservations.erase(found);
            }
        }
    }
    VkResult result = state->queueSubmit(queue, submitCount, submits, fence);
    CompleteRedirectReservations(*state, reservations, fence, result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL LayerQueueSubmit2(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* submits,
    VkFence fence) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDeviceFromObject(queue);
    if (!state || !state->queueSubmit2) return VK_ERROR_EXTENSION_NOT_PRESENT;
    std::vector<RedirectReservation> reservations;
    for (uint32_t submitIndex = 0; submitIndex < submitCount; ++submitIndex) {
        for (uint32_t bufferIndex = 0;
             bufferIndex < submits[submitIndex].commandBufferInfoCount;
             ++bufferIndex) {
            VkCommandBuffer buffer = submits[submitIndex]
                                          .pCommandBufferInfos[bufferIndex]
                                          .commandBuffer;
            auto found = state->reservations.find(buffer);
            if (found != state->reservations.end()) {
                reservations.push_back(found->second);
                state->reservations.erase(found);
            }
        }
    }
    VkResult result = state->queueSubmit2(queue, submitCount, submits, fence);
    CompleteRedirectReservations(*state, reservations, fence, result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL LayerQueueSubmit2KHR(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* submits,
    VkFence fence) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDeviceFromObject(queue);
    if (!state || !state->queueSubmit2KHR) return VK_ERROR_EXTENSION_NOT_PRESENT;
    std::vector<RedirectReservation> reservations;
    for (uint32_t submitIndex = 0; submitIndex < submitCount; ++submitIndex) {
        for (uint32_t bufferIndex = 0;
             bufferIndex < submits[submitIndex].commandBufferInfoCount;
             ++bufferIndex) {
            VkCommandBuffer buffer = submits[submitIndex]
                                          .pCommandBufferInfos[bufferIndex]
                                          .commandBuffer;
            auto found = state->reservations.find(buffer);
            if (found != state->reservations.end()) {
                reservations.push_back(found->second);
                state->reservations.erase(found);
            }
        }
    }
    VkResult result = state->queueSubmit2KHR(queue, submitCount, submits, fence);
    CompleteRedirectReservations(*state, reservations, fence, result);
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL LayerWaitForFences(
    VkDevice device, uint32_t fenceCount, const VkFence* fences, VkBool32 waitAll,
    uint64_t timeout) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    if (!state || !state->waitForFences) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result =
        state->waitForFences(device, fenceCount, fences, waitAll, timeout);
    if (result == VK_SUCCESS) {
        for (uint32_t index = 0; index < fenceCount; ++index)
            RetireFence(*state, fences[index], result);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL LayerDestroyFence(
    VkDevice device, VkFence fence, const VkAllocationCallbacks* allocator) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    if (!state || !state->destroyFence) return;
    RetireFence(*state, fence, VK_SUCCESS);
    state->destroyFence(device, fence, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL LayerCreateCommandPool(
    VkDevice device, const VkCommandPoolCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkCommandPool* pool) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    if (!state || !state->createCommandPool)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = state->createCommandPool(device, info, allocator, pool);
    if (result == VK_SUCCESS) state->pools[*pool] = {};
    return result;
}

VKAPI_ATTR void VKAPI_CALL LayerDestroyCommandPool(
    VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    if (!state || !state->destroyCommandPool) return;
    auto found = state->pools.find(pool);
    if (found != state->pools.end()) {
        for (VkCommandBuffer buffer : found->second) {
            auto reservation = state->reservations.find(buffer);
            if (reservation != state->reservations.end()) {
                Abandon(reservation->second, "OBS command pool destroyed");
                state->reservations.erase(reservation);
            }
            state->commandPools.erase(buffer);
        }
        state->pools.erase(found);
    }
    state->destroyCommandPool(device, pool, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL LayerResetCommandPool(
    VkDevice device, VkCommandPool pool, VkCommandPoolResetFlags flags) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    if (!state || !state->resetCommandPool)
        return VK_ERROR_INITIALIZATION_FAILED;
    auto found = state->pools.find(pool);
    if (found != state->pools.end()) {
        for (VkCommandBuffer buffer : found->second) {
            auto reservation = state->reservations.find(buffer);
            if (reservation != state->reservations.end()) {
                Abandon(reservation->second, "OBS command pool reset before submit");
                state->reservations.erase(reservation);
            }
        }
    }
    return state->resetCommandPool(device, pool, flags);
}

VKAPI_ATTR VkResult VKAPI_CALL LayerAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo* info,
    VkCommandBuffer* buffers) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    if (!state || !state->allocateCommandBuffers)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkResult result = state->allocateCommandBuffers(device, info, buffers);
    if (result == VK_SUCCESS) {
        auto& pool = state->pools[info->commandPool];
        for (uint32_t index = 0; index < info->commandBufferCount; ++index) {
            pool.insert(buffers[index]);
            state->commandPools[buffers[index]] = info->commandPool;
        }
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL LayerFreeCommandBuffers(
    VkDevice device, VkCommandPool pool, uint32_t count,
    const VkCommandBuffer* buffers) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    if (!state || !state->freeCommandBuffers) return;
    for (uint32_t index = 0; index < count; ++index) {
        auto reservation = state->reservations.find(buffers[index]);
        if (reservation != state->reservations.end()) {
            Abandon(reservation->second, "OBS command buffer freed before submit");
            state->reservations.erase(reservation);
        }
        state->commandPools.erase(buffers[index]);
        auto found = state->pools.find(pool);
        if (found != state->pools.end()) found->second.erase(buffers[index]);
    }
    state->freeCommandBuffers(device, pool, count, buffers);
}

}  // namespace

extern "C" {

PFN_vkGetDeviceProcAddr VKAPI_CALL
ToolscreenVulkanLayerGetDeviceProcAddr(VkDevice device) {
    std::lock_guard lock(g_mutex);
    DeviceState* state = FindDevice(device);
    return state ? state->gdpa : nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(
    VkNegotiateLayerInterface* version) {
    if (!version || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
        return VK_ERROR_INITIALIZATION_FAILED;
    version->loaderLayerInterfaceVersion =
        (std::min)(version->loaderLayerInterfaceVersion, 2u);
    version->pfnGetInstanceProcAddr = LayerGetInstanceProcAddr;
    version->pfnGetDeviceProcAddr = LayerGetDeviceProcAddr;
    version->pfnGetPhysicalDeviceProcAddr = nullptr;
    if (!g_loggedLoad.exchange(true)) {
        Log("module loaded; negotiation complete in PID=" +
            std::to_string(GetCurrentProcessId()) + ".");
    }
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
    VkInstance instance, const char* name) {
    return LayerGetInstanceProcAddr(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(
    VkDevice device, const char* name) {
    return LayerGetDeviceProcAddr(device, name);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator,
    VkInstance* instance) {
    return LayerCreateInstance(info, allocator, instance);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(
    VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkDevice* device) {
    return LayerCreateDevice(physicalDevice, info, allocator, device);
}

#ifndef TOOLSCREEN_VULKAN_LAYER_EMBEDDED
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(module);
    return TRUE;
}
#endif

}
