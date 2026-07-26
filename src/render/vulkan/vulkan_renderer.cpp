#include "vulkan_renderer.h"

#include "common/profiler.h"
#include "common/utils.h"
#include "gui/gui.h"
#include "gui/imgui_input_queue.h"
#include "hooks/input_hook.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_win32.h"
#include "render/render.h"
#include "render/render_backend.h"
#include "render/vulkan/vulkan_hooks.h"
#include "runtime/logic_thread.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern bool SubclassGameWindow(HWND hwnd);
extern std::atomic<bool> g_configLoaded;

namespace {

using namespace VulkanHooks;

constexpr uint32_t kQueriesPerFrame = 4;
constexpr uint32_t kMaxTimestampFrames = 16;
constexpr uint32_t kMaxMirrorQueriesPerFrame = 64;
constexpr VkDeviceSize kPickerReadbackStride = 16;

const uint32_t kMirrorVertexShader[] =
#include "shaders/mirror_vert.spv.inc"
;
const uint32_t kMirrorFragmentShader[] =
#include "shaders/mirror_frag.spv.inc"
;

struct MirrorSpecialization {
    int32_t targetCount = 0;
    float sensitivity = 0.001f;
    int32_t colorPassthrough = 0;
    int32_t dynamicBorderWidth = 0;
    float sourceTexelX = 0.0f;
    float sourceTexelY = 0.0f;
    float cropMinU = 0.0f;
    float cropMinV = 0.0f;
    float cropMaxU = 1.0f;
    float cropMaxV = 1.0f;
    float output[4] = { 1, 1, 1, 1 };
    float border[4] = { 0, 0, 0, 1 };
    float targets[8][3]{};
    int32_t gammaMode = 0;
};

struct TimestampFrame {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    uint32_t firstQuery = 0;
    bool pending = false;
    bool pickerPending = false;
    int pickerX = -1;
    int pickerY = -1;
    VkFormat pickerFormat = VK_FORMAT_UNDEFINED;
    uint32_t mirrorFirstQuery = 0;
    uint32_t mirrorQueryCount = 0;
    std::array<uint64_t, kMaxMirrorQueriesPerFrame> mirrorKeys{};
};

struct SampledImage {
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    VkImageLayout descriptorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct RendererState {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = VK_QUEUE_FAMILY_IGNORED;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t imageCount = 0;
    DeviceDispatch dispatch{};

    ImGuiContext* imguiContext = nullptr;
    HWND hwnd = NULL;
    VkSampler mirrorSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout mirrorDescriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout mirrorPipelineLayout = VK_NULL_HANDLE;
    VkShaderModule mirrorVertexShader = VK_NULL_HANDLE;
    VkShaderModule mirrorFragmentShader = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkPipeline> mirrorPipelines;
    PFN_vkCreateShaderModule createShaderModule = nullptr;
    PFN_vkDestroyShaderModule destroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines createGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
    PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer = nullptr;
    VkBuffer pickerReadbackBuffer = VK_NULL_HANDLE;
    VkDeviceMemory pickerReadbackMemory = VK_NULL_HANDLE;
    uint8_t* pickerReadbackMapped = nullptr;
    uintptr_t pickerTextureId = 0;
    int pickerFrameWidth = 0;
    int pickerFrameHeight = 0;
    bool pickerSampleRequested = false;
    int pickerRequestX = -1;
    int pickerRequestY = -1;
    bool pickerSampleReady = false;
    int pickerReadyX = -1;
    int pickerReadyY = -1;
    std::array<float, 4> pickerReadyColor{};
    VkImage fontImage = VK_NULL_HANDLE;
    VkDeviceMemory fontMemory = VK_NULL_HANDLE;
    VkImageView fontView = VK_NULL_HANDLE;
    VkDescriptorSet fontDescriptor = VK_NULL_HANDLE;
    VkBuffer fontUploadBuffer = VK_NULL_HANDLE;
    VkDeviceMemory fontUploadMemory = VK_NULL_HANDLE;
    void* fontUploadMapped = nullptr;
    uint32_t fontWidth = 0;
    uint32_t fontHeight = 0;
    bool fontUploadRecorded = false;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    VkQueryPool mirrorQueryPool = VK_NULL_HANDLE;
    PFN_vkCmdBeginQuery cmdBeginQuery = nullptr;
    PFN_vkCmdEndQuery cmdEndQuery = nullptr;
    std::unordered_map<uint64_t, bool> mirrorHasContent;
    float timestampPeriodNs = 1.0f;
    std::array<TimestampFrame, kMaxTimestampFrames> timestamps{};
    uint32_t nextTimestamp = 0;

    std::unordered_map<VkImage, SampledImage> imageResources;
    std::shared_ptr<const Config> configSnapshot;
    uint64_t configVersion = 0;
    std::string modeId;
    std::vector<MirrorConfig> mirrors;
    std::vector<ImageConfig> images;
    std::vector<const WindowOverlayConfig*> windowOverlays;
    std::vector<const BrowserOverlayConfig*> browserOverlays;
    bool initialized = false;
};

RendererState g_state;
std::atomic<bool> g_ready{ false };
std::atomic<bool> g_deviceBeingDestroyed{ false };
thread_local VkCommandBuffer g_activeImGuiCommandBuffer = VK_NULL_HANDLE;

VkImageMemoryBarrier MakeBarrier(VkImage image, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                                 VkImageLayout oldLayout, VkImageLayout newLayout);

void CheckVkResult(VkResult result) {
    if (result < 0) {
        Log("[VULKAN] ImGui backend error: " + std::to_string(static_cast<int>(result)));
    }
}

HWND FindMinecraftWindow() {
    HWND known = g_minecraftHwnd.load(std::memory_order_acquire);
    if (known && IsWindow(known)) return known;

    HWND foreground = GetForegroundWindow();
    if (foreground) {
        DWORD pid = 0;
        GetWindowThreadProcessId(foreground, &pid);
        if (pid == GetCurrentProcessId()) return foreground;
    }

    struct FindData {
        DWORD pid;
        HWND result;
    } data{ GetCurrentProcessId(), NULL };
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto* data = reinterpret_cast<FindData*>(param);
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == data->pid && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == NULL) {
            data->result = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));
    return data.result;
}

PFN_vkVoidFunction ImGuiLoader(const char* name, void* userData) {
    return VulkanHooks::LoadRealFunction(name, userData);
}

bool FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required, uint32_t& result) {
    auto getMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        VulkanHooks::LoadRealFunction("vkGetPhysicalDeviceMemoryProperties", reinterpret_cast<void*>(g_state.device)));
    if (!getMemoryProperties) return false;
    VkPhysicalDeviceMemoryProperties properties{};
    getMemoryProperties(g_state.physicalDevice, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & required) == required) {
            result = i;
            return true;
        }
    }
    return false;
}

bool CreateFontResources() {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (!pixels || width <= 0 || height <= 0) return false;
    const VkDeviceSize uploadSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;

    VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!g_state.dispatch.createImage ||
        g_state.dispatch.createImage(g_state.device, &imageInfo, nullptr, &g_state.fontImage) != VK_SUCCESS) return false;

    VkMemoryRequirements imageRequirements{};
    g_state.dispatch.getImageMemoryRequirements(g_state.device, g_state.fontImage, &imageRequirements);
    uint32_t imageMemoryType = 0;
    if (!FindMemoryType(imageRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, imageMemoryType)) return false;
    VkMemoryAllocateInfo imageAllocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    imageAllocation.allocationSize = imageRequirements.size;
    imageAllocation.memoryTypeIndex = imageMemoryType;
    if (g_state.dispatch.allocateMemory(g_state.device, &imageAllocation, nullptr, &g_state.fontMemory) != VK_SUCCESS ||
        g_state.dispatch.bindImageMemory(g_state.device, g_state.fontImage, g_state.fontMemory, 0) != VK_SUCCESS) return false;

    VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = g_state.fontImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = imageInfo.format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (g_state.dispatch.createImageView(g_state.device, &viewInfo, nullptr, &g_state.fontView) != VK_SUCCESS) return false;

    VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = uploadSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!g_state.dispatch.createBuffer ||
        g_state.dispatch.createBuffer(g_state.device, &bufferInfo, nullptr, &g_state.fontUploadBuffer) != VK_SUCCESS) return false;
    VkMemoryRequirements bufferRequirements{};
    g_state.dispatch.getBufferMemoryRequirements(g_state.device, g_state.fontUploadBuffer, &bufferRequirements);
    uint32_t uploadMemoryType = 0;
    if (!FindMemoryType(bufferRequirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        uploadMemoryType)) return false;
    VkMemoryAllocateInfo bufferAllocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    bufferAllocation.allocationSize = bufferRequirements.size;
    bufferAllocation.memoryTypeIndex = uploadMemoryType;
    if (g_state.dispatch.allocateMemory(g_state.device, &bufferAllocation, nullptr, &g_state.fontUploadMemory) != VK_SUCCESS ||
        g_state.dispatch.bindBufferMemory(g_state.device, g_state.fontUploadBuffer, g_state.fontUploadMemory, 0) != VK_SUCCESS ||
        g_state.dispatch.mapMemory(g_state.device, g_state.fontUploadMemory, 0, uploadSize, 0, &g_state.fontUploadMapped) != VK_SUCCESS) {
        return false;
    }
    memcpy(g_state.fontUploadMapped, pixels, static_cast<size_t>(uploadSize));
    g_state.fontWidth = static_cast<uint32_t>(width);
    g_state.fontHeight = static_cast<uint32_t>(height);
    g_state.fontDescriptor =
        ImGui_ImplVulkan_AddTexture(g_state.mirrorSampler, g_state.fontView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!g_state.fontDescriptor || !io.Fonts->TexData) return false;
    io.Fonts->TexData->SetTexID(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g_state.fontDescriptor)));
    io.Fonts->TexData->SetStatus(ImTextureStatus_OK);
    return true;
}

void RecordFontUpload(VkCommandBuffer commandBuffer) {
    if (g_state.fontUploadRecorded || !g_state.fontImage || !g_state.fontUploadBuffer) return;
    VkImageMemoryBarrier toTransfer = MakeBarrier(
        g_state.fontImage, VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    g_state.dispatch.cmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                        0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = { g_state.fontWidth, g_state.fontHeight, 1 };
    g_state.dispatch.cmdCopyBufferToImage(commandBuffer, g_state.fontUploadBuffer, g_state.fontImage,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier toShader = MakeBarrier(
        g_state.fontImage, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g_state.dispatch.cmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                        0, 0, nullptr, 0, nullptr, 1, &toShader);
    g_state.fontUploadRecorded = true;
}

bool IsColorPickerFormatSupported(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_R16G16B16A16_UNORM:
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        return true;
    default:
        return false;
    }
}

float HalfToFloat(uint16_t half) {
    const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
    uint32_t exponent = (half >> 10) & 0x1fu;
    uint32_t mantissa = half & 0x03ffu;
    uint32_t value = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            value = sign;
        } else {
            exponent = 127u - 15u + 1u;
            while ((mantissa & 0x0400u) == 0) {
                mantissa <<= 1;
                --exponent;
            }
            mantissa &= 0x03ffu;
            value = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 31u) {
        value = sign | 0x7f800000u | (mantissa << 13);
    } else {
        value = sign | ((exponent + (127u - 15u)) << 23) | (mantissa << 13);
    }
    float result = 0.0f;
    memcpy(&result, &value, sizeof(result));
    return result;
}

bool DecodeColorPickerPixel(const uint8_t* bytes, VkFormat format, std::array<float, 4>& color) {
    if (!bytes) return false;
    constexpr float kByteScale = 1.0f / 255.0f;
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        color = { bytes[0] * kByteScale, bytes[1] * kByteScale,
                  bytes[2] * kByteScale, bytes[3] * kByteScale };
        return true;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        color = { bytes[2] * kByteScale, bytes[1] * kByteScale,
                  bytes[0] * kByteScale, bytes[3] * kByteScale };
        return true;
    case VK_FORMAT_R16G16B16A16_UNORM: {
        uint16_t components[4]{};
        memcpy(components, bytes, sizeof(components));
        constexpr float scale = 1.0f / 65535.0f;
        color = { components[0] * scale, components[1] * scale,
                  components[2] * scale, components[3] * scale };
        return true;
    }
    case VK_FORMAT_R16G16B16A16_SFLOAT: {
        uint16_t components[4]{};
        memcpy(components, bytes, sizeof(components));
        color = { std::clamp(HalfToFloat(components[0]), 0.0f, 1.0f),
                  std::clamp(HalfToFloat(components[1]), 0.0f, 1.0f),
                  std::clamp(HalfToFloat(components[2]), 0.0f, 1.0f),
                  std::clamp(HalfToFloat(components[3]), 0.0f, 1.0f) };
        return true;
    }
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
        uint32_t packed = 0;
        memcpy(&packed, bytes, sizeof(packed));
        constexpr float colorScale = 1.0f / 1023.0f;
        constexpr float alphaScale = 1.0f / 3.0f;
        const float low = static_cast<float>(packed & 0x3ffu) * colorScale;
        const float green = static_cast<float>((packed >> 10) & 0x3ffu) * colorScale;
        const float high = static_cast<float>((packed >> 20) & 0x3ffu) * colorScale;
        const float alpha = static_cast<float>((packed >> 30) & 0x3u) * alphaScale;
        color = format == VK_FORMAT_A2B10G10R10_UNORM_PACK32
            ? std::array<float, 4>{ low, green, high, alpha }
            : std::array<float, 4>{ high, green, low, alpha };
        return true;
    }
    default:
        return false;
    }
}

bool CreateColorPickerReadbackResources() {
    g_state.cmdCopyImageToBuffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
        VulkanHooks::LoadRealFunction("vkCmdCopyImageToBuffer", reinterpret_cast<void*>(g_state.device)));
    if (!g_state.cmdCopyImageToBuffer || !g_state.dispatch.createBuffer) return false;

    VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = kPickerReadbackStride * kMaxTimestampFrames;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_state.dispatch.createBuffer(
            g_state.device, &bufferInfo, nullptr, &g_state.pickerReadbackBuffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    g_state.dispatch.getBufferMemoryRequirements(
        g_state.device, g_state.pickerReadbackBuffer, &requirements);
    uint32_t memoryType = 0;
    if (!FindMemoryType(requirements.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        memoryType)) {
        return false;
    }
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (g_state.dispatch.allocateMemory(
            g_state.device, &allocation, nullptr, &g_state.pickerReadbackMemory) != VK_SUCCESS ||
        g_state.dispatch.bindBufferMemory(
            g_state.device, g_state.pickerReadbackBuffer, g_state.pickerReadbackMemory, 0) != VK_SUCCESS ||
        g_state.dispatch.mapMemory(
            g_state.device, g_state.pickerReadbackMemory, 0, bufferInfo.size, 0,
            reinterpret_cast<void**>(&g_state.pickerReadbackMapped)) != VK_SUCCESS) {
        return false;
    }
    return true;
}

bool CreateMirrorPipelineResources() {
    auto load = [](const char* name) {
        return VulkanHooks::LoadRealFunction(name, reinterpret_cast<void*>(g_state.device));
    };
    g_state.createShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(load("vkCreateShaderModule"));
    g_state.destroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(load("vkDestroyShaderModule"));
    g_state.createDescriptorSetLayout =
        reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(load("vkCreateDescriptorSetLayout"));
    g_state.destroyDescriptorSetLayout =
        reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(load("vkDestroyDescriptorSetLayout"));
    g_state.createPipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(load("vkCreatePipelineLayout"));
    g_state.destroyPipelineLayout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(load("vkDestroyPipelineLayout"));
    g_state.createGraphicsPipelines =
        reinterpret_cast<PFN_vkCreateGraphicsPipelines>(load("vkCreateGraphicsPipelines"));
    g_state.destroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(load("vkDestroyPipeline"));
    g_state.cmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(load("vkCmdBindPipeline"));
    if (!g_state.createShaderModule || !g_state.createDescriptorSetLayout || !g_state.createPipelineLayout ||
        !g_state.createGraphicsPipelines || !g_state.cmdBindPipeline) {
        return false;
    }

    VkShaderModuleCreateInfo vertexInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    vertexInfo.codeSize = sizeof(kMirrorVertexShader);
    vertexInfo.pCode = kMirrorVertexShader;
    if (g_state.createShaderModule(g_state.device, &vertexInfo, nullptr, &g_state.mirrorVertexShader) != VK_SUCCESS) {
        return false;
    }
    VkShaderModuleCreateInfo fragmentInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    fragmentInfo.codeSize = sizeof(kMirrorFragmentShader);
    fragmentInfo.pCode = kMirrorFragmentShader;
    if (g_state.createShaderModule(g_state.device, &fragmentInfo, nullptr, &g_state.mirrorFragmentShader) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo descriptorInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    descriptorInfo.bindingCount = 1;
    descriptorInfo.pBindings = &binding;
    if (g_state.createDescriptorSetLayout(
            g_state.device, &descriptorInfo, nullptr, &g_state.mirrorDescriptorSetLayout) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange pushConstant{};
    pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstant.size = sizeof(float) * 4;
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &g_state.mirrorDescriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushConstant;
    return g_state.createPipelineLayout(
               g_state.device, &layoutInfo, nullptr, &g_state.mirrorPipelineLayout) == VK_SUCCESS;
}

uint64_t HashMirrorSpecialization(const MirrorSpecialization& specialization) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&specialization);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof(specialization); ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

VkPipeline GetMirrorPipeline(const MirrorSpecialization& specialization) {
    static_assert(sizeof(MirrorSpecialization) == sizeof(uint32_t) * 43);
    const uint64_t key = HashMirrorSpecialization(specialization);
    if (const auto it = g_state.mirrorPipelines.find(key); it != g_state.mirrorPipelines.end()) {
        return it->second;
    }

    std::array<VkSpecializationMapEntry, 43> entries{};
    for (uint32_t i = 0; i < entries.size(); ++i) {
        entries[i].constantID = i;
        entries[i].offset = i * sizeof(uint32_t);
        entries[i].size = sizeof(uint32_t);
    }
    VkSpecializationInfo specializationInfo{};
    specializationInfo.mapEntryCount = static_cast<uint32_t>(entries.size());
    specializationInfo.pMapEntries = entries.data();
    specializationInfo.dataSize = sizeof(specialization);
    specializationInfo.pData = &specialization;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g_state.mirrorVertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g_state.mirrorFragmentShader;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = &specializationInfo;

    VkVertexInputBindingDescription vertexBinding{};
    vertexBinding.binding = 0;
    vertexBinding.stride = sizeof(ImDrawVert);
    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 3> attributes{};
    attributes[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, pos) };
    attributes[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, uv) };
    attributes[2] = { 2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col) };
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &vertexBinding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    const VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &g_state.format;
    VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.pNext = &rendering;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depth;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = g_state.mirrorPipelineLayout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (g_state.createGraphicsPipelines(
            g_state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    g_state.mirrorPipelines.emplace(key, pipeline);
    return pipeline;
}

void BindMirrorPipelineCallback(const ImDrawList*, const ImDrawCmd* command) {
    if (!g_activeImGuiCommandBuffer || !g_state.cmdBindPipeline) return;
    const VkPipeline pipeline = reinterpret_cast<VkPipeline>(command->UserCallbackData);
    if (pipeline) {
        g_state.cmdBindPipeline(g_activeImGuiCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    }
}

void BeginMirrorQueryCallback(const ImDrawList*, const ImDrawCmd* command) {
    if (!g_activeImGuiCommandBuffer || !g_state.cmdBeginQuery || !g_state.mirrorQueryPool) return;
    const uint32_t query = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(command->UserCallbackData) - 1u);
    g_state.cmdBeginQuery(
        g_activeImGuiCommandBuffer, g_state.mirrorQueryPool, query, 0);
}

void EndMirrorQueryCallback(const ImDrawList*, const ImDrawCmd* command) {
    if (!g_activeImGuiCommandBuffer || !g_state.cmdEndQuery || !g_state.mirrorQueryPool) return;
    const uint32_t query = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(command->UserCallbackData) - 1u);
    g_state.cmdEndQuery(g_activeImGuiCommandBuffer, g_state.mirrorQueryPool, query);
}

uint64_t HashMirrorIdentity(const std::string& name) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char value : name) {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool CreateImageView(VkImage image, const ImageMetadata& metadata, SampledImage& result) {
    if (!g_state.dispatch.createImageView) return false;
    VkImageViewCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = metadata.format;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.layerCount = 1;
    return g_state.dispatch.createImageView(g_state.device, &info, nullptr, &result.view) == VK_SUCCESS;
}

SampledImage* GetSampledImage(VkImage image, const ImageMetadata& metadata, VkImageLayout layout) {
    auto [it, inserted] = g_state.imageResources.try_emplace(image);
    SampledImage& resource = it->second;
    if (inserted && !CreateImageView(image, metadata, resource)) {
        g_state.imageResources.erase(it);
        return nullptr;
    }
    if (resource.descriptor == VK_NULL_HANDLE || resource.descriptorLayout != layout) {
        if (resource.descriptor != VK_NULL_HANDLE) ImGui_ImplVulkan_RemoveTexture(resource.descriptor);
        resource.descriptor = ImGui_ImplVulkan_AddTexture(g_state.mirrorSampler, resource.view, layout);
        resource.descriptorLayout = layout;
    }
    return resource.descriptor != VK_NULL_HANDLE ? &resource : nullptr;
}

SampledImage* GetImageView(VkImage image, const ImageMetadata& metadata) {
    auto [it, inserted] = g_state.imageResources.try_emplace(image);
    if (inserted && !CreateImageView(image, metadata, it->second)) {
        g_state.imageResources.erase(it);
        return nullptr;
    }
    return &it->second;
}

bool InitializeRenderer(const VulkanRenderer::FinalBlitContext& context) {
    auto snapshot = VulkanHooks::GetSnapshot();
    if (!context.swapchain || !context.dispatch || !context.destinationMetadata || snapshot->instance == VK_NULL_HANDLE) return false;

    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = VK_QUEUE_FAMILY_IGNORED;
    for (const auto& [candidate, metadata] : snapshot->queues) {
        if (metadata.device == context.device) {
            queue = candidate;
            family = metadata.familyIndex;
            break;
        }
    }
    if (!queue || family == VK_QUEUE_FAMILY_IGNORED) return false;

    HWND hwnd = FindMinecraftWindow();
    if (!hwnd) return false;

    g_state.instance = snapshot->instance;
    g_state.device = context.device;
    g_state.physicalDevice = context.dispatch->physicalDevice;
    g_state.queue = queue;
    g_state.queueFamily = family;
    g_state.format = context.swapchain->format;
    g_state.imageCount = static_cast<uint32_t>(context.swapchain->images.size());
    g_state.dispatch = *context.dispatch;
    g_state.hwnd = hwnd;

    if (g_state.imageCount < 2) g_state.imageCount = (std::max)(2u, context.swapchain->minImageCount);

    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, ImGuiLoader, reinterpret_cast<void*>(g_state.device))) {
        Log("[VULKAN] ImGui could not load the Vulkan device dispatch.");
        return false;
    }

    IMGUI_CHECKVERSION();
    g_state.imguiContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_state.imguiContext);
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ConfigureImGuiFontsAndStyleForCurrentContext(ComputeGuiScaleFactorFromCachedWindowSize());
    if (!ImGui_ImplWin32_Init(hwnd)) return false;

    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_3;
    init.Instance = g_state.instance;
    init.PhysicalDevice = g_state.physicalDevice;
    init.Device = g_state.device;
    init.QueueFamily = g_state.queueFamily;
    init.Queue = g_state.queue;
    init.DescriptorPoolSize = 256;
    init.MinImageCount = (std::max)(2u, context.swapchain->minImageCount);
    init.ImageCount = g_state.imageCount;
    init.UseDynamicRendering = true;
    init.CheckVkResultFn = CheckVkResult;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_state.format;
    if (!ImGui_ImplVulkan_Init(&init)) return false;

    // The stock Vulkan backend advertises dynamic texture updates and expects
    // ImGui_ImplVulkan_RenderDrawData() to service the font-atlas requests.
    // Toolscreen instead owns a persistent, nonblocking atlas upload recorded
    // into Minecraft's final command buffer. Keep this context on ImGui's
    // legacy/static atlas contract so NewFrame() does not replace or invalidate
    // the texture data and descriptor published by CreateFontResources().
    ImGui::GetIO().BackendFlags &= ~ImGuiBackendFlags_RendererHasTextures;

    VkSamplerCreateInfo sampler{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 0.0f;
    if (!g_state.dispatch.createSampler ||
        g_state.dispatch.createSampler(g_state.device, &sampler, nullptr, &g_state.mirrorSampler) != VK_SUCCESS) {
        return false;
    }
    if (!CreateFontResources()) {
        Log("[VULKAN] Failed to create the persistent ImGui font upload resources.");
        return false;
    }
    if (!CreateMirrorPipelineResources()) {
        Log("[VULKAN] Failed to create the filtered mirror pipeline resources.");
        return false;
    }
    if (!CreateColorPickerReadbackResources()) {
        Log("[VULKAN] Color-picker readback resources are unavailable.");
    }

    if (g_state.dispatch.createQueryPool) {
        VkQueryPoolCreateInfo queryInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = kMaxTimestampFrames * kQueriesPerFrame;
        g_state.dispatch.createQueryPool(g_state.device, &queryInfo, nullptr, &g_state.queryPool);

        queryInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
        queryInfo.queryCount = kMaxTimestampFrames * kMaxMirrorQueriesPerFrame;
        g_state.dispatch.createQueryPool(
            g_state.device, &queryInfo, nullptr, &g_state.mirrorQueryPool);
    }
    g_state.cmdBeginQuery = reinterpret_cast<PFN_vkCmdBeginQuery>(
        VulkanHooks::LoadRealFunction("vkCmdBeginQuery", reinterpret_cast<void*>(g_state.device)));
    g_state.cmdEndQuery = reinterpret_cast<PFN_vkCmdEndQuery>(
        VulkanHooks::LoadRealFunction("vkCmdEndQuery", reinterpret_cast<void*>(g_state.device)));
    if (auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            VulkanHooks::LoadRealFunction("vkGetPhysicalDeviceProperties", reinterpret_cast<void*>(g_state.device)))) {
        VkPhysicalDeviceProperties properties{};
        getProperties(g_state.physicalDevice, &properties);
        g_state.timestampPeriodNs = properties.limits.timestampPeriod;
    }

    g_minecraftHwnd.store(hwnd, std::memory_order_release);
    SubclassGameWindow(hwnd);
    g_state.initialized = true;
    g_ready.store(true, std::memory_order_release);
    LogCategory("init", "[VULKAN] Native dynamic-rendering backend initialized.");
    return true;
}

void HarvestTimestamps() {
    if (!g_state.queryPool || !g_state.dispatch.getQueryPoolResults) return;
    for (TimestampFrame& frame : g_state.timestamps) {
        if (!frame.pending) continue;
        std::array<uint64_t, kQueriesPerFrame * 2> values{};
        VkResult result = g_state.dispatch.getQueryPoolResults(
            g_state.device, g_state.queryPool, frame.firstQuery, kQueriesPerFrame,
            sizeof(values), values.data(), sizeof(uint64_t) * 2,
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result != VK_SUCCESS) continue;
        bool allAvailable = true;
        for (uint32_t i = 0; i < kQueriesPerFrame; ++i) {
            if (values[i * 2 + 1] == 0) allAvailable = false;
        }
        if (!allAvailable) continue;

        if (frame.pickerPending && g_state.pickerReadbackMapped) {
            const uint32_t frameIndex = frame.firstQuery / kQueriesPerFrame;
            std::array<float, 4> decoded{};
            if (DecodeColorPickerPixel(
                    g_state.pickerReadbackMapped + frameIndex * kPickerReadbackStride,
                    frame.pickerFormat, decoded)) {
                g_state.pickerReadyX = frame.pickerX;
                g_state.pickerReadyY = frame.pickerY;
                g_state.pickerReadyColor = decoded;
                g_state.pickerSampleReady = true;
            }
        }
        if (frame.mirrorQueryCount > 0 && g_state.mirrorQueryPool) {
            std::array<uint64_t, kMaxMirrorQueriesPerFrame> samples{};
            const VkResult mirrorResult = g_state.dispatch.getQueryPoolResults(
                g_state.device, g_state.mirrorQueryPool, frame.mirrorFirstQuery,
                frame.mirrorQueryCount, sizeof(uint64_t) * frame.mirrorQueryCount,
                samples.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
            if (mirrorResult == VK_SUCCESS) {
                for (uint32_t i = 0; i < frame.mirrorQueryCount; ++i) {
                    g_state.mirrorHasContent[frame.mirrorKeys[i]] = samples[i] != 0;
                }
            }
        }
        const double periodMs = static_cast<double>(g_state.timestampPeriodNs) / 1'000'000.0;
        Profiler::GetInstance().SubmitExternalTiming(
            "Vulkan GPU final blit", static_cast<double>(values[2] - values[0]) * periodMs);
        Profiler::GetInstance().SubmitExternalTiming(
            "Vulkan GPU overlay", static_cast<double>(values[6] - values[4]) * periodMs);
        frame.pending = false;
        frame.pickerPending = false;
        frame.mirrorQueryCount = 0;
        frame.commandBuffer = VK_NULL_HANDLE;
    }
}

TimestampFrame* BeginTimestamps(VkCommandBuffer commandBuffer) {
    HarvestTimestamps();
    if (!g_state.queryPool || !g_state.dispatch.cmdResetQueryPool || !g_state.dispatch.cmdWriteTimestamp) return nullptr;
    for (uint32_t attempt = 0; attempt < kMaxTimestampFrames; ++attempt) {
        uint32_t index = (g_state.nextTimestamp + attempt) % kMaxTimestampFrames;
        TimestampFrame& frame = g_state.timestamps[index];
        if (frame.pending) continue;
        frame.commandBuffer = commandBuffer;
        frame.firstQuery = index * kQueriesPerFrame;
        frame.mirrorFirstQuery = index * kMaxMirrorQueriesPerFrame;
        frame.mirrorQueryCount = 0;
        frame.pending = true;
        g_state.nextTimestamp = (index + 1) % kMaxTimestampFrames;
        g_state.dispatch.cmdResetQueryPool(commandBuffer, g_state.queryPool, frame.firstQuery, kQueriesPerFrame);
        g_state.dispatch.cmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                            g_state.queryPool, frame.firstQuery);
        return &frame;
    }
    return nullptr;
}

void WriteTimestamp(VkCommandBuffer commandBuffer, TimestampFrame* frame, uint32_t relativeIndex,
                    VkPipelineStageFlagBits stage) {
    if (frame && g_state.dispatch.cmdWriteTimestamp) {
        g_state.dispatch.cmdWriteTimestamp(commandBuffer, stage, g_state.queryPool,
                                            frame->firstQuery + relativeIndex);
    }
}

ModeViewportInfo ResolveSubmittedViewport(const VulkanRenderer::FinalBlitContext& context) {
    ModeViewportInfo viewport{};
    if (!ResolvePresentedGameViewport(viewport)) {
        viewport = GetCurrentModeViewport();
    }

    const int swapW = static_cast<int>(context.swapchain->extent.width);
    const int swapH = static_cast<int>(context.swapchain->extent.height);
    int x = viewport.valid ? viewport.stretchX : 0;
    int y = viewport.valid ? viewport.stretchY : 0;
    int width = viewport.valid ? viewport.stretchWidth : swapW;
    int height = viewport.valid ? viewport.stretchHeight : swapH;
    x = std::clamp(x, 0, swapW);
    y = std::clamp(y, 0, swapH);
    width = std::clamp(width, 0, swapW - x);
    height = std::clamp(height, 0, swapH - y);
    viewport.valid = width > 0 && height > 0;
    viewport.x = x;
    viewport.y = y;
    viewport.width = width;
    viewport.height = height;
    viewport.stretchX = x;
    viewport.stretchY = y;
    viewport.stretchWidth = width;
    viewport.stretchHeight = height;
    return viewport;
}

VkImageBlit BuildAdjustedBlit(const VulkanRenderer::FinalBlitContext& context) {
    VkImageBlit adjusted{};
    if (context.regionCount && context.regions) adjusted = context.regions[0];
    adjusted.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    if (adjusted.srcSubresource.layerCount == 0) adjusted.srcSubresource.layerCount = 1;
    adjusted.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    if (adjusted.dstSubresource.layerCount == 0) adjusted.dstSubresource.layerCount = 1;

    const int sourceW = context.sourceMetadata ? static_cast<int>(context.sourceMetadata->extent.width)
                                               : (std::max)(1, adjusted.srcOffsets[1].x - adjusted.srcOffsets[0].x);
    const int sourceH = context.sourceMetadata ? static_cast<int>(context.sourceMetadata->extent.height)
                                               : (std::max)(1, adjusted.srcOffsets[1].y - adjusted.srcOffsets[0].y);
    adjusted.srcOffsets[0] = { 0, 0, 0 };
    adjusted.srcOffsets[1] = { sourceW, sourceH, 1 };

    PROFILE_SCOPE_CAT("Vulkan viewport calculation", "Vulkan");
    const ModeViewportInfo viewport = ResolveSubmittedViewport(context);
    const int x = viewport.x;
    const int y = viewport.y;
    const int w = viewport.width;
    const int h = viewport.height;

    // Vulkan framebuffer coordinates are top-left. Minecraft's presentation
    // blit is vertically flipped, so retain the reversed destination Y pair.
    adjusted.dstOffsets[0] = { x, y + h, 0 };
    adjusted.dstOffsets[1] = { x + w, y, 1 };
    return adjusted;
}

VkImageMemoryBarrier MakeBarrier(VkImage image, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                                 VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    return barrier;
}

void RefreshModeCache(int screenW, int screenH) {
    const uint64_t version = g_configSnapshotVersion.load(std::memory_order_acquire);
    const std::string mode = GetPublishedCurrentModeId();
    if (g_state.configSnapshot && version == g_state.configVersion && mode == g_state.modeId) return;
    g_state.configSnapshot = GetConfigSnapshot();
    g_state.configVersion = version;
    g_state.modeId = mode;
    g_state.mirrors.clear();
    g_state.mirrorHasContent.clear();
    g_state.images.clear();
    g_state.windowOverlays.clear();
    g_state.browserOverlays.clear();
    if (g_state.configSnapshot) {
        CollectActiveElementsForMode(*g_state.configSnapshot, mode, false, version, g_state.mirrors, g_state.images,
                                     g_state.windowOverlays, g_state.browserOverlays, screenW, screenH);
    }
}

Color InterpolateBackgroundColor(const BackgroundConfig& background, float value, bool seamless) {
    const size_t stopCount = (std::min)(background.gradientStops.size(), size_t{ 8 });
    if (stopCount < 2) return background.color;
    float t = seamless ? value - std::floor(value) : std::clamp(value, 0.0f, 1.0f);
    const GradientColorStop& first = background.gradientStops.front();
    const GradientColorStop& last = background.gradientStops[stopCount - 1];

    if (seamless && (t < first.position || t > last.position)) {
        const float wrapLength = (1.0f - last.position) + first.position;
        if (wrapLength > 0.0001f) {
            const float amount = t < first.position
                ? (t + 1.0f - last.position) / wrapLength
                : (t - last.position) / wrapLength;
            return {
                last.color.r + (first.color.r - last.color.r) * amount,
                last.color.g + (first.color.g - last.color.g) * amount,
                last.color.b + (first.color.b - last.color.b) * amount,
                last.color.a + (first.color.a - last.color.a) * amount,
            };
        }
    }

    if (t <= first.position) return first.color;
    for (size_t i = 0; i + 1 < stopCount; ++i) {
        const GradientColorStop& left = background.gradientStops[i];
        const GradientColorStop& right = background.gradientStops[i + 1];
        if (t <= right.position) {
            const float amount = (t - left.position) / (std::max)(0.0001f, right.position - left.position);
            return {
                left.color.r + (right.color.r - left.color.r) * amount,
                left.color.g + (right.color.g - left.color.g) * amount,
                left.color.b + (right.color.b - left.color.b) * amount,
                left.color.a + (right.color.a - left.color.a) * amount,
            };
        }
    }
    return last.color;
}

ImU32 EvaluateBackgroundGradient(const BackgroundConfig& background, float u, float v, float elapsedSeconds) {
    constexpr float kPi = 3.14159265358979323846f;
    const float angle = background.gradientAngle * kPi / 180.0f;
    const float speedTime = elapsedSeconds * background.gradientAnimationSpeed;
    const float centeredX = u - 0.5f;
    const float centeredY = v - 0.5f;
    float effectiveAngle = angle;
    float t = 0.0f;
    bool seamless = false;

    switch (background.gradientAnimation) {
    case GradientAnimationType::Rotate:
        effectiveAngle += speedTime;
        t = centeredX * std::cos(effectiveAngle) + centeredY * std::sin(effectiveAngle) + 0.5f;
        break;
    case GradientAnimationType::Slide:
        t = centeredX * std::cos(angle) + centeredY * std::sin(angle) + 0.5f + speedTime * 0.2f;
        seamless = true;
        break;
    case GradientAnimationType::Wave: {
        const float along = centeredX * std::cos(angle) + centeredY * std::sin(angle);
        const float across = centeredX * -std::sin(angle) + centeredY * std::cos(angle);
        t = along + 0.5f + std::sin(across * 8.0f + speedTime * 2.0f) * 0.08f;
        break;
    }
    case GradientAnimationType::Spiral:
        t = std::sqrt(centeredX * centeredX + centeredY * centeredY) * 2.0f +
            std::atan2(centeredY, centeredX) / (2.0f * kPi) - speedTime * 0.3f;
        seamless = true;
        break;
    case GradientAnimationType::Fade:
        t = speedTime * 0.1f;
        seamless = true;
        break;
    case GradientAnimationType::None:
    default:
        t = centeredX * std::cos(angle) + centeredY * std::sin(angle) + 0.5f;
        break;
    }

    if (!seamless && background.gradientColorFade) {
        t = t + speedTime * 0.1f;
        seamless = true;
    }
    const Color color = InterpolateBackgroundColor(background, t, seamless);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(color.r, color.g, color.b, color.a));
}

Color InterpolateMirrorGradient(const GradientConfig& gradient, float value, bool seamless,
                                const Color& fallback) {
    const size_t stopCount = (std::min)(gradient.gradientStops.size(), size_t{ 8 });
    if (stopCount < 2) return fallback;
    const float t = seamless ? value - std::floor(value) : std::clamp(value, 0.0f, 1.0f);
    const GradientColorStop& first = gradient.gradientStops.front();
    const GradientColorStop& last = gradient.gradientStops[stopCount - 1];

    if (seamless && (t < first.position || t > last.position)) {
        const float wrapLength = (1.0f - last.position) + first.position;
        if (wrapLength > 0.0001f) {
            const float amount = t < first.position
                ? (t + 1.0f - last.position) / wrapLength
                : (t - last.position) / wrapLength;
            return {
                last.color.r + (first.color.r - last.color.r) * amount,
                last.color.g + (first.color.g - last.color.g) * amount,
                last.color.b + (first.color.b - last.color.b) * amount,
                last.color.a + (first.color.a - last.color.a) * amount,
            };
        }
    }

    if (t <= first.position) return first.color;
    for (size_t i = 0; i + 1 < stopCount; ++i) {
        const GradientColorStop& left = gradient.gradientStops[i];
        const GradientColorStop& right = gradient.gradientStops[i + 1];
        if (t <= right.position) {
            const float amount = (t - left.position) /
                                 (std::max)(0.0001f, right.position - left.position);
            return {
                left.color.r + (right.color.r - left.color.r) * amount,
                left.color.g + (right.color.g - left.color.g) * amount,
                left.color.b + (right.color.b - left.color.b) * amount,
                left.color.a + (right.color.a - left.color.a) * amount,
            };
        }
    }
    return last.color;
}

ImU32 EvaluateMirrorGradient(const MirrorConfig& mirror, float u, float v, float elapsedSeconds) {
    constexpr float kPi = 3.14159265358979323846f;
    const GradientConfig& gradient = mirror.gradient;
    const float angle = gradient.gradientAngle * kPi / 180.0f;
    const float speedTime = elapsedSeconds * gradient.gradientAnimationSpeed;
    const float centeredX = u - 0.5f;
    const float centeredY = v - 0.5f;
    float t = 0.0f;
    bool seamless = false;

    switch (gradient.gradientAnimation) {
    case GradientAnimationType::Rotate: {
        const float effectiveAngle = angle + speedTime;
        t = centeredX * std::cos(effectiveAngle) + centeredY * std::sin(effectiveAngle) + 0.5f;
        break;
    }
    case GradientAnimationType::Slide:
        t = centeredX * std::cos(angle) + centeredY * std::sin(angle) + 0.5f + speedTime * 0.2f;
        seamless = true;
        break;
    case GradientAnimationType::Wave: {
        const float along = centeredX * std::cos(angle) + centeredY * std::sin(angle);
        const float across = centeredX * -std::sin(angle) + centeredY * std::cos(angle);
        t = along + 0.5f + std::sin(across * 8.0f + speedTime * 2.0f) * 0.08f;
        break;
    }
    case GradientAnimationType::Spiral:
        t = std::sqrt(centeredX * centeredX + centeredY * centeredY) * 2.0f +
            std::atan2(centeredY, centeredX) / (2.0f * kPi) - speedTime * 0.3f;
        seamless = true;
        break;
    case GradientAnimationType::Fade:
        t = speedTime * 0.1f;
        seamless = true;
        break;
    case GradientAnimationType::None:
    default:
        t = centeredX * std::cos(angle) + centeredY * std::sin(angle) + 0.5f;
        break;
    }

    if (!seamless && gradient.gradientColorFade) {
        t += speedTime * 0.1f;
        seamless = true;
    }
    const Color color = InterpolateMirrorGradient(gradient, t, seamless, mirror.colors.output);
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4(color.r, color.g, color.b, color.a * std::clamp(mirror.opacity, 0.0f, 1.0f)));
}

void AddImageMultiColor(ImDrawList* draw, ImTextureID texture, const ImVec2& minimum,
                        const ImVec2& maximum, const ImVec2& uv0, const ImVec2& uv1,
                        ImU32 topLeft, ImU32 topRight, ImU32 bottomRight, ImU32 bottomLeft) {
    const ImTextureRef textureRef(texture);
    draw->PushTexture(textureRef);
    draw->PrimReserve(6, 4);
    const ImDrawIdx index = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
    draw->PrimWriteIdx(index);
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(index + 1));
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(index + 2));
    draw->PrimWriteIdx(index);
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(index + 2));
    draw->PrimWriteIdx(static_cast<ImDrawIdx>(index + 3));
    draw->PrimWriteVtx(minimum, uv0, topLeft);
    draw->PrimWriteVtx(ImVec2(maximum.x, minimum.y), ImVec2(uv1.x, uv0.y), topRight);
    draw->PrimWriteVtx(maximum, uv1, bottomRight);
    draw->PrimWriteVtx(ImVec2(minimum.x, maximum.y), ImVec2(uv0.x, uv1.y), bottomLeft);
    draw->PopTexture();
}

void DrawModeBackground(const VulkanRenderer::FinalBlitContext& context, const ModeViewportInfo& viewport) {
    if (!g_state.configSnapshot || !viewport.valid) return;
    const ModeConfig* mode =
        GetModeFromSnapshotOrFallback(*g_state.configSnapshot, g_state.modeId);
    if (!mode) return;

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float screenW = static_cast<float>(context.swapchain->extent.width);
    const float screenH = static_cast<float>(context.swapchain->extent.height);
    const float left = static_cast<float>(viewport.x);
    const float top = static_cast<float>(viewport.y);
    const float right = static_cast<float>(viewport.x + viewport.width);
    const float bottom = static_cast<float>(viewport.y + viewport.height);
    const BackgroundConfig& background = mode->background;

    if (background.selectedMode != "gradient" || background.gradientStops.size() < 2) {
        const ImU32 color = ImGui::ColorConvertFloat4ToU32(
            ImVec4(background.color.r, background.color.g, background.color.b, background.color.a));
        if (top > 0.0f) draw->AddRectFilled(ImVec2(0, 0), ImVec2(screenW, top), color);
        if (bottom < screenH) draw->AddRectFilled(ImVec2(0, bottom), ImVec2(screenW, screenH), color);
        if (left > 0.0f) draw->AddRectFilled(ImVec2(0, top), ImVec2(left, bottom), color);
        if (right < screenW) draw->AddRectFilled(ImVec2(right, top), ImVec2(screenW, bottom), color);
        return;
    }

    static const auto start = std::chrono::steady_clock::now();
    const float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
    std::array<float, 36> xCuts{};
    std::array<float, 22> yCuts{};
    size_t xCount = 0;
    size_t yCount = 0;
    for (int i = 0; i <= 32; ++i) xCuts[xCount++] = screenW * static_cast<float>(i) / 32.0f;
    for (int i = 0; i <= 18; ++i) yCuts[yCount++] = screenH * static_cast<float>(i) / 18.0f;
    xCuts[xCount++] = left;
    xCuts[xCount++] = right;
    yCuts[yCount++] = top;
    yCuts[yCount++] = bottom;
    std::sort(xCuts.begin(), xCuts.begin() + xCount);
    std::sort(yCuts.begin(), yCuts.begin() + yCount);

    for (size_t yi = 0; yi + 1 < yCount; ++yi) {
        const float y0 = yCuts[yi];
        const float y1 = yCuts[yi + 1];
        if (y1 <= y0) continue;
        for (size_t xi = 0; xi + 1 < xCount; ++xi) {
            const float x0 = xCuts[xi];
            const float x1 = xCuts[xi + 1];
            if (x1 <= x0) continue;
            if (x0 >= left && x1 <= right && y0 >= top && y1 <= bottom) continue;
            draw->AddRectFilledMultiColor(
                ImVec2(x0, y0), ImVec2(x1, y1),
                EvaluateBackgroundGradient(background, x0 / screenW, y0 / screenH, elapsed),
                EvaluateBackgroundGradient(background, x1 / screenW, y0 / screenH, elapsed),
                EvaluateBackgroundGradient(background, x1 / screenW, y1 / screenH, elapsed),
                EvaluateBackgroundGradient(background, x0 / screenW, y1 / screenH, elapsed));
        }
    }
}

bool PrepareMirrorSource(const VulkanRenderer::FinalBlitContext& context, VkImageLayout& sampleLayout,
                         SampledImage*& sampled) {
    PROFILE_SCOPE_CAT("Vulkan mirror preparation", "Vulkan");
    sampled = nullptr;
    if (!context.sourceMetadata ||
        (g_state.mirrors.empty() && !g_showGui.load(std::memory_order_acquire))) {
        return false;
    }
    if ((context.sourceMetadata->usage & VK_IMAGE_USAGE_SAMPLED_BIT) == 0) {
        // The GPU copy fallback is intentionally selected by capability. Its
        // owned sampled image is created lazily by the texture-upload path.
        return false;
    }
    sampleLayout = context.sourceLayout == VK_IMAGE_LAYOUT_GENERAL
                       ? VK_IMAGE_LAYOUT_GENERAL
                       : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sampled = GetSampledImage(context.sourceImage, *context.sourceMetadata, sampleLayout);
    if (!sampled) return false;

    VkImageMemoryBarrier barrier = MakeBarrier(
        context.sourceImage, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
        context.sourceLayout, sampleLayout);
    g_state.dispatch.cmdPipelineBarrier(
        context.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    return true;
}

void DrawMirrors(const VulkanRenderer::FinalBlitContext& context, SampledImage* sampled,
                 TimestampFrame* timestampFrame) {
    PROFILE_SCOPE_CAT("Vulkan mirror rendering", "Vulkan");
    if (!sampled || !context.sourceMetadata) return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float sourceW = static_cast<float>(context.sourceMetadata->extent.width);
    const float sourceH = static_cast<float>(context.sourceMetadata->extent.height);
    ModeViewportInfo viewport = ResolveSubmittedViewport(context);
    const int screenW = static_cast<int>(context.swapchain->extent.width);
    const int screenH = static_cast<int>(context.swapchain->extent.height);
    static const auto gradientStart = std::chrono::steady_clock::now();
    const float gradientElapsed =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - gradientStart).count();

    for (const MirrorConfig& mirror : g_state.mirrors) {
        if (mirror.input.empty() || mirror.opacity <= 0.0f) continue;
        const uint64_t mirrorKey = HashMirrorIdentity(mirror.name);
        bool hasFrameContent = mirror.rawOutput;
        if (!mirror.rawOutput) {
            const auto content = g_state.mirrorHasContent.find(mirrorKey);
            hasFrameContent = content != g_state.mirrorHasContent.end() && content->second;
        }
        const MirrorCaptureConfig& input = mirror.input.front();
        int captureX = 0;
        int captureY = 0;
        GetRelativeCoords(input.relativeTo, input.x, input.y, mirror.captureWidth, mirror.captureHeight,
                          static_cast<int>(sourceW), static_cast<int>(sourceH), captureX, captureY);
        captureX = std::clamp(captureX, 0, static_cast<int>(sourceW));
        captureY = std::clamp(captureY, 0, static_cast<int>(sourceH));
        const int captureW = std::clamp(mirror.captureWidth, 0, static_cast<int>(sourceW) - captureX);
        const int captureH = std::clamp(mirror.captureHeight, 0, static_cast<int>(sourceH) - captureY);
        if (captureW <= 0 || captureH <= 0) continue;

        const float scaleX = mirror.output.separateScale ? mirror.output.scaleX : mirror.output.scale;
        const float scaleY = mirror.output.separateScale ? mirror.output.scaleY : mirror.output.scale;
        const int outputW = (std::max)(1, static_cast<int>(captureW * scaleX));
        const int outputH = (std::max)(1, static_cast<int>(captureH * scaleY));
        int outputX = 0;
        int outputY = 0;
        const bool viewportRelative = mirror.output.relativeTo.ends_with("Viewport");
        const int anchorW = viewportRelative && viewport.valid ? viewport.width : screenW;
        const int anchorH = viewportRelative && viewport.valid ? viewport.height : screenH;
        GetRelativeCoords(mirror.output.relativeTo, mirror.output.x, mirror.output.y, outputW, outputH,
                          anchorW, anchorH, outputX, outputY);
        if (viewportRelative && viewport.valid) {
            outputX += viewport.x;
            outputY += viewport.y;
        }

        // Minecraft's final blit maps source Y=0 to the bottom of the
        // swapchain. Mirror capture coordinates are configured in presented
        // top-left screen space, so invert both the crop origin and V
        // direction when sampling the unpresented source image directly.
        const ImVec2 uv0(static_cast<float>(captureX) / sourceW,
                         1.0f - (static_cast<float>(captureY) / sourceH));
        const ImVec2 uv1(static_cast<float>(captureX + captureW) / sourceW,
                         1.0f - (static_cast<float>(captureY + captureH) / sourceH));
        const ImTextureID texture =
            static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(sampled->descriptor));
        const ImVec2 outputMinimum(static_cast<float>(outputX), static_cast<float>(outputY));
        const ImVec2 outputMaximum(static_cast<float>(outputX + outputW),
                                   static_cast<float>(outputY + outputH));

        if (mirror.rawOutput) {
            const ImU32 tint = IM_COL32(
                255, 255, 255,
                static_cast<int>(std::clamp(mirror.opacity, 0.0f, 1.0f) * 255.0f));
            draw->AddImage(texture, outputMinimum, outputMaximum, uv0, uv1, tint);
        } else {
            MirrorSpecialization specialization{};
            specialization.targetCount = static_cast<int32_t>(
                (std::min)(mirror.colors.targetColors.size(), size_t{ 8 }));
            specialization.sensitivity = mirror.colorSensitivity;
            specialization.colorPassthrough = mirror.colorPassthrough ? 1 : 0;
            specialization.dynamicBorderWidth =
                mirror.border.type == MirrorBorderType::Dynamic
                    ? std::clamp(mirror.border.dynamicThickness, 0, 16)
                    : 0;
            // Dynamic border thickness is expressed in output pixels. Convert
            // one output pixel to source UV space so scaling does not also
            // scale the configured border thickness.
            specialization.sourceTexelX =
                std::abs(uv1.x - uv0.x) / static_cast<float>((std::max)(1, outputW));
            specialization.sourceTexelY =
                std::abs(uv1.y - uv0.y) / static_cast<float>((std::max)(1, outputH));
            specialization.cropMinU = (std::min)(uv0.x, uv1.x);
            specialization.cropMinV = (std::min)(uv0.y, uv1.y);
            specialization.cropMaxU = (std::max)(uv0.x, uv1.x);
            specialization.cropMaxV = (std::max)(uv0.y, uv1.y);
            // Solid and gradient colors are supplied through ImGui's vertex
            // colors. Keep the shader multiplier white; outputA is also the
            // passthrough opacity.
            specialization.output[0] = 1.0f;
            specialization.output[1] = 1.0f;
            specialization.output[2] = 1.0f;
            specialization.output[3] = mirror.colorPassthrough
                ? std::clamp(mirror.opacity, 0.0f, 1.0f)
                : (!mirror.gradientOutput
                       ? mirror.colors.output.a * std::clamp(mirror.opacity, 0.0f, 1.0f)
                       : 1.0f);
            specialization.border[0] = mirror.colors.border.r;
            specialization.border[1] = mirror.colors.border.g;
            specialization.border[2] = mirror.colors.border.b;
            specialization.border[3] =
                mirror.colors.border.a * std::clamp(mirror.opacity, 0.0f, 1.0f);
            for (int32_t i = 0; i < specialization.targetCount; ++i) {
                const Color& target = mirror.colors.targetColors[static_cast<size_t>(i)];
                specialization.targets[i][0] = target.r;
                specialization.targets[i][1] = target.g;
                specialization.targets[i][2] = target.b;
            }
            specialization.gammaMode = g_state.configSnapshot
                ? static_cast<int32_t>(g_state.configSnapshot->mirrorGammaMode)
                : static_cast<int32_t>(MirrorGammaMode::Auto);

            const VkPipeline pipeline = GetMirrorPipeline(specialization);
            if (pipeline) {
                draw->AddCallback(BindMirrorPipelineCallback,
                                  reinterpret_cast<void*>(pipeline));
                uint32_t contentQuery = UINT32_MAX;
                if (timestampFrame && g_state.mirrorQueryPool &&
                    g_state.cmdBeginQuery && g_state.cmdEndQuery &&
                    timestampFrame->mirrorQueryCount < kMaxMirrorQueriesPerFrame) {
                    const uint32_t localQuery = timestampFrame->mirrorQueryCount++;
                    timestampFrame->mirrorKeys[localQuery] = mirrorKey;
                    contentQuery = timestampFrame->mirrorFirstQuery + localQuery;
                    draw->AddCallback(
                        BeginMirrorQueryCallback,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(contentQuery) + 1u));
                }
                if (mirror.colorPassthrough) {
                    draw->AddImage(texture, outputMinimum, outputMaximum, uv0, uv1, IM_COL32_WHITE);
                } else if (mirror.gradientOutput && mirror.gradient.gradientStops.size() >= 2) {
                    AddImageMultiColor(
                        draw, texture, outputMinimum, outputMaximum, uv0, uv1,
                        EvaluateMirrorGradient(mirror, 0.0f, 0.0f, gradientElapsed),
                        EvaluateMirrorGradient(mirror, 1.0f, 0.0f, gradientElapsed),
                        EvaluateMirrorGradient(mirror, 1.0f, 1.0f, gradientElapsed),
                        EvaluateMirrorGradient(mirror, 0.0f, 1.0f, gradientElapsed));
                } else {
                    const Color& color = mirror.colors.output;
                    const ImU32 outputColor = ImGui::ColorConvertFloat4ToU32(
                        // Keep vertex alpha nonzero so the draw (and therefore
                        // its content query) is emitted even when the selected
                        // output color is fully transparent. Shader outputA
                        // carries the configured opacity.
                        ImVec4(color.r, color.g, color.b, 1.0f));
                    draw->AddImage(texture, outputMinimum, outputMaximum, uv0, uv1, outputColor);
                }
                if (contentQuery != UINT32_MAX) {
                    draw->AddCallback(
                        EndMirrorQueryCallback,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(contentQuery) + 1u));
                }
                draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            }
        }

        const MirrorBorderConfig& border = mirror.border;
        if (border.type == MirrorBorderType::Static && border.staticThickness > 0 &&
            hasFrameContent) {
            const Color& color = border.staticColor;
            const float borderWidth = static_cast<float>(
                border.staticWidth > 0 ? border.staticWidth : outputW);
            const float borderHeight = static_cast<float>(
                border.staticHeight > 0 ? border.staticHeight : outputH);
            const float borderX = static_cast<float>(outputX + border.staticOffsetX) +
                                  (static_cast<float>(outputW) - borderWidth) * 0.5f;
            const float borderY = static_cast<float>(outputY + border.staticOffsetY) +
                                  (static_cast<float>(outputH) - borderHeight) * 0.5f;
            const ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(
                ImVec4(color.r, color.g, color.b,
                       color.a * std::clamp(mirror.opacity, 0.0f, 1.0f)));
            if (border.staticShape == MirrorBorderShape::Circle) {
                draw->AddEllipse(
                    ImVec2(borderX + borderWidth * 0.5f, borderY + borderHeight * 0.5f),
                    ImVec2(borderWidth * 0.5f, borderHeight * 0.5f), borderColor, 0.0f, 0,
                    static_cast<float>(border.staticThickness));
            } else {
                draw->AddRect(
                    ImVec2(borderX, borderY), ImVec2(borderX + borderWidth, borderY + borderHeight),
                    borderColor, static_cast<float>(border.staticRadius), 0,
                    static_cast<float>(border.staticThickness));
            }
        }
    }
}

void GenerateImGui(const VulkanRenderer::FinalBlitContext& context, SampledImage* mirrorSource,
                   TimestampFrame* timestampFrame) {
    PROFILE_SCOPE_CAT("Vulkan ImGui generation", "Vulkan");
    ImGui::SetCurrentContext(g_state.imguiContext);
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(context.swapchain->extent.width),
                            static_cast<float>(context.swapchain->extent.height));
    ImGuiInputQueue_DrainToImGui();
    ImGui::NewFrame();
    g_state.pickerTextureId = mirrorSource
        ? reinterpret_cast<uintptr_t>(mirrorSource->descriptor)
        : 0;
    g_state.pickerFrameWidth = context.sourceMetadata
        ? static_cast<int>(context.sourceMetadata->extent.width)
        : 0;
    g_state.pickerFrameHeight = context.sourceMetadata
        ? static_cast<int>(context.sourceMetadata->extent.height)
        : 0;

    if (g_configLoadFailed.load(std::memory_order_acquire)) {
        RenderConfigErrorGUI();
    } else {
        DrawModeBackground(context, ResolveSubmittedViewport(context));
        DrawMirrors(context, mirrorSource, timestampFrame);
        if (g_state.configSnapshot) {
            const bool showPerformance = g_state.configSnapshot->debug.showPerformanceOverlay;
            const bool showProfiler = g_state.configSnapshot->debug.showProfiler;
            if (showPerformance) RenderPerformanceOverlay(true);
            if (showProfiler) RenderProfilerOverlay(true, showPerformance);
        }
        if (g_showGui.load(std::memory_order_acquire)) RenderSettingsGUI();
    }
    ImGui::Render();
}

void RestoreMirrorSource(const VulkanRenderer::FinalBlitContext& context, VkImageLayout sampleLayout,
                         SampledImage* sampled) {
    if (!sampled) return;
    VkImageMemoryBarrier barrier = MakeBarrier(
        context.sourceImage, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        sampleLayout, context.sourceLayout);
    g_state.dispatch.cmdPipelineBarrier(
        context.commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void RecordColorPickerSample(const VulkanRenderer::FinalBlitContext& context, TimestampFrame* frame) {
    if (!g_state.pickerSampleRequested) return;
    g_state.pickerSampleRequested = false;
    if (!frame || !g_state.pickerReadbackBuffer || !g_state.cmdCopyImageToBuffer ||
        !context.sourceMetadata || !IsColorPickerFormatSupported(context.sourceMetadata->format) ||
        (context.sourceMetadata->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
        return;
    }

    const int width = static_cast<int>(context.sourceMetadata->extent.width);
    const int height = static_cast<int>(context.sourceMetadata->extent.height);
    const int x = std::clamp(g_state.pickerRequestX, 0, (std::max)(0, width - 1));
    const int presentedY = std::clamp(g_state.pickerRequestY, 0, (std::max)(0, height - 1));
    const int sourceY = height - presentedY - 1;
    const uint32_t frameIndex = frame->firstQuery / kQueriesPerFrame;

    VkBufferImageCopy copy{};
    copy.bufferOffset = frameIndex * kPickerReadbackStride;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset = { x, sourceY, 0 };
    copy.imageExtent = { 1, 1, 1 };
    g_state.cmdCopyImageToBuffer(
        context.commandBuffer, context.sourceImage, context.sourceLayout,
        g_state.pickerReadbackBuffer, 1, &copy);

    VkBufferMemoryBarrier toHost{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = g_state.pickerReadbackBuffer;
    toHost.offset = copy.bufferOffset;
    toHost.size = kPickerReadbackStride;
    g_state.dispatch.cmdPipelineBarrier(
        context.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, nullptr, 1, &toHost, 0, nullptr);

    frame->pickerPending = true;
    frame->pickerX = x;
    frame->pickerY = presentedY;
    frame->pickerFormat = context.sourceMetadata->format;
}

} // namespace

namespace VulkanRenderer {

bool RecordAfterFinalBlit(const FinalBlitContext& context, PFN_vkCmdBlitImage originalBlit) {
    if (!originalBlit || !context.dispatch || !context.swapchain || !context.destinationMetadata) return false;
    if (!TryLatchRenderBackend(RenderBackend::Vulkan)) return false;
    if (g_deviceBeingDestroyed.load(std::memory_order_acquire)) return false;
    if (!g_state.initialized && !InitializeRenderer(context)) {
        // Still emit Minecraft's adjusted blit even if optional overlay resources
        // are not ready yet.
        VkImageBlit adjusted = BuildAdjustedBlit(context);
        originalBlit(context.commandBuffer, context.sourceImage, context.sourceLayout, context.destinationImage,
                     context.destinationLayout, 1, &adjusted, context.filter);
        return true;
    }
    if (g_state.device != context.device || g_state.format != context.swapchain->format) {
        Log("[VULKAN] Device or swapchain format changed; deferring overlay until resources are recreated.");
        return false;
    }

    if (!g_logicThreadRunning.load(std::memory_order_acquire) && g_configLoaded.load(std::memory_order_acquire)) {
        StartLogicThread();
    }
    if (!g_configLoaded.load(std::memory_order_acquire) &&
        !g_configLoadFailed.load(std::memory_order_acquire)) {
        return false;
    }

    g_state.dispatch = *context.dispatch;
    if (g_state.hwnd != g_minecraftHwnd.load(std::memory_order_acquire)) {
        HWND hwnd = FindMinecraftWindow();
        if (hwnd && hwnd != g_state.hwnd) {
            g_state.hwnd = hwnd;
            g_minecraftHwnd.store(hwnd, std::memory_order_release);
            SubclassGameWindow(hwnd);
        }
    }

    const Config* cfg = nullptr;
    auto cfgSnapshot = GetConfigSnapshot();
    if (cfgSnapshot) cfg = cfgSnapshot.get();
    const bool profilerEnabled = cfg && cfg->debug.showProfiler;
    Profiler::GetInstance().SetEnabled(profilerEnabled);
    if (profilerEnabled) Profiler::GetInstance().MarkAsRenderThread();

    TimestampFrame* timestamp = BeginTimestamps(context.commandBuffer);
    VkImageBlit adjusted = BuildAdjustedBlit(context);
    originalBlit(context.commandBuffer, context.sourceImage, context.sourceLayout, context.destinationImage,
                 context.destinationLayout, 1, &adjusted, context.filter);
    WriteTimestamp(context.commandBuffer, timestamp, 1, VK_PIPELINE_STAGE_TRANSFER_BIT);

    if ((context.destinationMetadata->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        return true;
    }

    RefreshModeCache(static_cast<int>(context.swapchain->extent.width), static_cast<int>(context.swapchain->extent.height));

    VkImageLayout mirrorSampleLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    SampledImage* mirrorSource = nullptr;
    PrepareMirrorSource(context, mirrorSampleLayout, mirrorSource);
    RecordFontUpload(context.commandBuffer);

    VkImageMemoryBarrier toColor = MakeBarrier(
        context.destinationImage, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    context.dispatch->cmdPipelineBarrier(
        context.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toColor);

    SampledImage* destinationView = GetImageView(context.destinationImage, *context.destinationMetadata);
    if (!destinationView) {
        RestoreMirrorSource(context, mirrorSampleLayout, mirrorSource);
        return true;
    }

    {
        PROFILE_SCOPE_CAT("Vulkan resource upload", "Vulkan");
        GenerateImGui(context, mirrorSource, timestamp);
    }
    WriteTimestamp(context.commandBuffer, timestamp, 2, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    if (timestamp && timestamp->mirrorQueryCount > 0 &&
        g_state.mirrorQueryPool && g_state.dispatch.cmdResetQueryPool) {
        g_state.dispatch.cmdResetQueryPool(
            context.commandBuffer, g_state.mirrorQueryPool,
            timestamp->mirrorFirstQuery, timestamp->mirrorQueryCount);
    }

    {
        PROFILE_SCOPE_CAT("Vulkan overlay recording", "Vulkan");
        VkRenderingAttachmentInfo color{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        color.imageView = destinationView->view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo rendering{ VK_STRUCTURE_TYPE_RENDERING_INFO };
        rendering.renderArea.extent = context.swapchain->extent;
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;

        if (context.dispatch->cmdBeginRendering && context.dispatch->cmdEndRendering) {
            context.dispatch->cmdBeginRendering(context.commandBuffer, &rendering);
            g_activeImGuiCommandBuffer = context.commandBuffer;
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), context.commandBuffer);
            g_activeImGuiCommandBuffer = VK_NULL_HANDLE;
            context.dispatch->cmdEndRendering(context.commandBuffer);
        } else if (context.dispatch->cmdBeginRenderingKHR && context.dispatch->cmdEndRenderingKHR) {
            context.dispatch->cmdBeginRenderingKHR(context.commandBuffer, &rendering);
            g_activeImGuiCommandBuffer = context.commandBuffer;
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), context.commandBuffer);
            g_activeImGuiCommandBuffer = VK_NULL_HANDLE;
            context.dispatch->cmdEndRenderingKHR(context.commandBuffer);
        }
    }
    VkImageMemoryBarrier toTransfer = MakeBarrier(
        context.destinationImage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    context.dispatch->cmdPipelineBarrier(
        context.commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    RestoreMirrorSource(context, mirrorSampleLayout, mirrorSource);
    RecordColorPickerSample(context, timestamp);
    WriteTimestamp(context.commandBuffer, timestamp, 3, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    Profiler::GetInstance().EndFrame();
    return true;
}

bool IsReady() {
    return g_ready.load(std::memory_order_acquire);
}

bool GetColorPickerFrame(uintptr_t& textureId, int& width, int& height) {
    if (!g_state.initialized || !g_state.pickerTextureId ||
        g_state.pickerFrameWidth <= 0 || g_state.pickerFrameHeight <= 0) {
        return false;
    }
    textureId = g_state.pickerTextureId;
    width = g_state.pickerFrameWidth;
    height = g_state.pickerFrameHeight;
    return true;
}

void RequestColorPickerSample(int x, int y) {
    if (!g_state.initialized) return;
    g_state.pickerRequestX = x;
    g_state.pickerRequestY = y;
    g_state.pickerSampleRequested = true;
}

bool TryGetColorPickerSample(int x, int y, std::array<float, 4>& color) {
    RequestColorPickerSample(x, y);
    if (!g_state.pickerSampleReady ||
        g_state.pickerReadyX != x || g_state.pickerReadyY != y) {
        return false;
    }
    color = g_state.pickerReadyColor;
    return true;
}

void OnQueueSubmit(VkDevice, VkQueue, uint32_t, const VkCommandBuffer*, VkFence) {
    // Timestamp availability is polled without VK_QUERY_RESULT_WAIT_BIT on a
    // later frame. No fence wait or second overlay submission is introduced.
}

void OnImageDestroyed(VkDevice device, VkImage image) {
    if (!g_state.initialized || g_state.device != device || !image) return;
    auto it = g_state.imageResources.find(image);
    if (it == g_state.imageResources.end()) return;
    ImGui::SetCurrentContext(g_state.imguiContext);
    if (it->second.descriptor) ImGui_ImplVulkan_RemoveTexture(it->second.descriptor);
    if (it->second.view && g_state.dispatch.destroyImageView) {
        g_state.dispatch.destroyImageView(device, it->second.view, nullptr);
    }
    g_state.imageResources.erase(it);
}

void OnSwapchainDestroyed(VkDevice device, VkSwapchainKHR, const std::vector<VkImage>& images) {
    if (!g_state.initialized || g_state.device != device) return;
    for (VkImage image : images) OnImageDestroyed(device, image);
}

void OnDeviceDestroyed(VkDevice device) {
    if (g_state.device != device) return;
    g_deviceBeingDestroyed.store(true, std::memory_order_release);
    Shutdown();
}

void Shutdown() {
    if (!g_state.initialized) return;
    g_ready.store(false, std::memory_order_release);
    ImGui::SetCurrentContext(g_state.imguiContext);
    for (auto& [image, resource] : g_state.imageResources) {
        if (resource.descriptor) ImGui_ImplVulkan_RemoveTexture(resource.descriptor);
        if (resource.view && g_state.dispatch.destroyImageView) {
            g_state.dispatch.destroyImageView(g_state.device, resource.view, nullptr);
        }
    }
    g_state.imageResources.clear();
    if (g_state.fontDescriptor) ImGui_ImplVulkan_RemoveTexture(g_state.fontDescriptor);
    if (g_state.queryPool && g_state.dispatch.destroyQueryPool) {
        g_state.dispatch.destroyQueryPool(g_state.device, g_state.queryPool, nullptr);
    }
    if (g_state.mirrorQueryPool && g_state.dispatch.destroyQueryPool) {
        g_state.dispatch.destroyQueryPool(g_state.device, g_state.mirrorQueryPool, nullptr);
    }
    if (g_state.destroyPipeline) {
        for (const auto& [key, pipeline] : g_state.mirrorPipelines) {
            if (pipeline) g_state.destroyPipeline(g_state.device, pipeline, nullptr);
        }
    }
    g_state.mirrorPipelines.clear();
    if (g_state.mirrorVertexShader && g_state.destroyShaderModule) {
        g_state.destroyShaderModule(g_state.device, g_state.mirrorVertexShader, nullptr);
    }
    if (g_state.mirrorFragmentShader && g_state.destroyShaderModule) {
        g_state.destroyShaderModule(g_state.device, g_state.mirrorFragmentShader, nullptr);
    }
    if (g_state.mirrorPipelineLayout && g_state.destroyPipelineLayout) {
        g_state.destroyPipelineLayout(g_state.device, g_state.mirrorPipelineLayout, nullptr);
    }
    if (g_state.mirrorDescriptorSetLayout && g_state.destroyDescriptorSetLayout) {
        g_state.destroyDescriptorSetLayout(g_state.device, g_state.mirrorDescriptorSetLayout, nullptr);
    }
    if (g_state.mirrorSampler && g_state.dispatch.destroySampler) {
        g_state.dispatch.destroySampler(g_state.device, g_state.mirrorSampler, nullptr);
    }
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(g_state.imguiContext);
    if (g_state.fontUploadMapped && g_state.dispatch.unmapMemory) {
        g_state.dispatch.unmapMemory(g_state.device, g_state.fontUploadMemory);
    }
    if (g_state.pickerReadbackMapped && g_state.dispatch.unmapMemory) {
        g_state.dispatch.unmapMemory(g_state.device, g_state.pickerReadbackMemory);
    }
    if (g_state.pickerReadbackBuffer && g_state.dispatch.destroyBuffer) {
        g_state.dispatch.destroyBuffer(g_state.device, g_state.pickerReadbackBuffer, nullptr);
    }
    if (g_state.pickerReadbackMemory && g_state.dispatch.freeMemory) {
        g_state.dispatch.freeMemory(g_state.device, g_state.pickerReadbackMemory, nullptr);
    }
    if (g_state.fontUploadBuffer && g_state.dispatch.destroyBuffer) {
        g_state.dispatch.destroyBuffer(g_state.device, g_state.fontUploadBuffer, nullptr);
    }
    if (g_state.fontUploadMemory && g_state.dispatch.freeMemory) {
        g_state.dispatch.freeMemory(g_state.device, g_state.fontUploadMemory, nullptr);
    }
    if (g_state.fontView && g_state.dispatch.destroyImageView) {
        g_state.dispatch.destroyImageView(g_state.device, g_state.fontView, nullptr);
    }
    if (g_state.fontImage && g_state.dispatch.destroyImage) {
        g_state.dispatch.destroyImage(g_state.device, g_state.fontImage, nullptr);
    }
    if (g_state.fontMemory && g_state.dispatch.freeMemory) {
        g_state.dispatch.freeMemory(g_state.device, g_state.fontMemory, nullptr);
    }
    g_state = {};
    g_deviceBeingDestroyed.store(false, std::memory_order_release);
}

} // namespace VulkanRenderer
