#include "vulkan_renderer.h"

#include "common/profiler.h"
#include "common/i18n.h"
#include "common/utils.h"
#include "features/browser_overlay.h"
#include "features/fake_cursor.h"
#include "features/virtual_camera.h"
#include "features/window_overlay.h"
#include "gui/gui.h"
#include "gui/imgui_input_queue.h"
#include "hooks/input_hook.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_win32.h"
#include "render/render.h"
#include "render/background_fit_layout.h"
#include "render/obs_thread.h"
#include "render/render_backend.h"
#include "render/vulkan/vulkan_hooks.h"
#include "render/vulkan/vulkan_obs_redirect_api.h"
#include "runtime/logic_thread.h"
#include "platform/resource.h"
#include "third_party/stb_image.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern bool SubclassGameWindow(HWND hwnd);
extern std::atomic<bool> g_configLoaded;

namespace {

using namespace VulkanHooks;

constexpr uint32_t kQueriesPerFrame = 6;
constexpr uint32_t kMaxTimestampFrames = 16;
constexpr uint32_t kMaxMirrorQueriesPerFrame = 64;
constexpr uint32_t kObsCompositionSlotCount = 4;
constexpr VkDeviceSize kPickerReadbackStride = 16;
constexpr int kEyeZoomSnapshotFpsKey = 9999;
constexpr const char* kRebindIndicatorEnabledTextureId =
    "__vulkan_rebind_indicator_enabled";
constexpr const char* kRebindIndicatorDisabledTextureId =
    "__vulkan_rebind_indicator_disabled";
constexpr const char* kCursorTrailTextureId =
    "__vulkan_cursor_trail";
constexpr const char* kStartupIndicatorTextureId =
    "__vulkan_startup_indicator";
constexpr const char* kGuiLanguageTextureId = "__vulkan_gui_language";
constexpr const char* kGuiDiscordTextureId = "__vulkan_gui_discord";
constexpr const char* kGuiEditorTextureId = "__vulkan_gui_editor";
constexpr std::array<const char*, 4> kNinjabrainBoatTextureIds = {
    "__vulkan_ninjabrain_boat_gray",
    "__vulkan_ninjabrain_boat_blue",
    "__vulkan_ninjabrain_boat_green",
    "__vulkan_ninjabrain_boat_red",
};
constexpr std::array<const char*, 3> kNinjabrainMessageTextureIds = {
    "__vulkan_ninjabrain_info",
    "__vulkan_ninjabrain_warning",
    "__vulkan_ninjabrain_lock",
};
constexpr std::array<int, 4> kNinjabrainBoatResourceIds = {
    IDR_BOAT_GRAY, IDR_BOAT_BLUE, IDR_BOAT_GREEN, IDR_BOAT_RED,
};
constexpr std::array<int, 3> kNinjabrainMessageResourceIds = {
    IDR_NINJABRAIN_INFO_ICON,
    IDR_NINJABRAIN_WARNING_ICON,
    IDR_NINJABRAIN_LOCK_ICON,
};

const uint32_t kMirrorVertexShader[] =
#include "shaders/mirror_vert.spv.inc"
;
const uint32_t kMirrorFragmentShader[] =
#include "shaders/mirror_frag.spv.inc"
;
const uint32_t kOverlayFragmentShader[] =
#include "shaders/overlay_frag.spv.inc"
;
const uint32_t kVirtualCameraComputeShader[] =
#include "shaders/virtual_camera_comp.spv.inc"
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
    int32_t gradientStopCount = 0;
    float gradientColors[8][4]{};
    float gradientPositions[8]{};
    float gradientAngle = 0.0f;
    int32_t gradientAnimationType = 0;
    float gradientAnimationSpeed = 1.0f;
    int32_t gradientColorFade = 0;
    int32_t staticBorderMode = 0;
    int32_t staticBorderShape = 0;
};

struct MirrorFragmentPushConstants {
    float gradientTime = 0.0f;
    float padding[3]{};
    float staticBorderColor[4] = { 0, 0, 0, 0 };
    float staticBorderThickness = 0.0f;
    float staticBorderRadius = 0.0f;
    float staticBorderSize[2] = { 0, 0 };
    float staticBorderQuadSize[2] = { 0, 0 };
};

struct OverlaySpecialization {
    int32_t keyCount = 0;
    float keys[8][3]{};
    float sensitivities[8]{};
};

enum class OverlayBlendMode : uint8_t {
    Alpha,
    Additive,
    Invert,
};

struct TimestampFrame {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    uint32_t firstQuery = 0;
    bool pending = false;
    bool pickerPending = false;
    int pickerX = -1;
    int pickerY = -1;
    VkFormat pickerFormat = VK_FORMAT_UNDEFINED;
    bool screenshotPending = false;
    int screenshotWidth = 0;
    int screenshotHeight = 0;
    VkFormat screenshotFormat = VK_FORMAT_UNDEFINED;
    bool virtualCameraPending = false;
    VirtualCameraGpuFrame virtualCameraFrame{};
    uint32_t virtualCameraCompositionSlot = UINT32_MAX;
    uint64_t virtualCameraCompositionSerial = 0;
    bool compositionWritePending = false;
    uint32_t compositionWriteSlot = UINT32_MAX;
    uint64_t compositionWriteSerial = 0;
    uint32_t mirrorFirstQuery = 0;
    uint32_t mirrorQueryCount = 0;
    std::array<uint64_t, kMaxMirrorQueriesPerFrame> mirrorKeys{};
};

struct SampledImage {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    VkDescriptorSet linearDescriptor = VK_NULL_HANDLE;
    VkImageLayout descriptorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct MirrorCopyImage {
    SampledImage sampled;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent3D extent{};
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool initialized = false;
};

struct MirrorSnapshotSlot {
    MirrorCopyImage image;
    uint32_t pendingFrameMask = 0;
};

struct MirrorSnapshotState {
    std::vector<MirrorSnapshotSlot> slots;
    size_t latestSlot = std::numeric_limits<size_t>::max();
    std::chrono::steady_clock::time_point lastUpdate{};
};

struct RetiredMirrorSnapshot {
    MirrorCopyImage image;
    uint32_t pendingFrameMask = 0;
};

struct TextureFrame {
    SampledImage sampled;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct TextureAsset {
    DecodedImageData::Type type = DecodedImageData::UserImage;
    int width = 0;
    int height = 0;
    bool isFullyTransparent = false;
    std::vector<TextureFrame> frames;
    std::vector<int> frameDelays;
    std::vector<uint64_t> frameEndTimesMs;
    uint64_t totalDurationMs = 0;
    std::chrono::steady_clock::time_point animationStart{};
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    VkDeviceSize stagingSize = 0;
    bool uploadRecorded = false;
};

struct RetiredTextureAsset {
    TextureAsset asset;
    uint32_t pendingFrameMask = 0;
};

struct RetiredFontResource {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet descriptor = VK_NULL_HANDLE;
    VkBuffer uploadBuffer = VK_NULL_HANDLE;
    VkDeviceMemory uploadMemory = VK_NULL_HANDLE;
    void* uploadMapped = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool uploadRecorded = false;
    uint32_t pendingFrameMask = 0;
};

struct StreamingTextureSlot {
    TextureFrame frame;
    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    void* stagingMapped = nullptr;
    VkDeviceSize stagingSize = 0;
    int width = 0;
    int height = 0;
    uint64_t uploadedGeneration = 0;
};

struct StreamingTexture {
    std::unordered_map<VkImage, StreamingTextureSlot> slots;
};

struct ObsCompositionSlot {
    MirrorCopyImage image;
    VkCommandBuffer recordingCommandBuffer = VK_NULL_HANDLE;
    uint64_t serial = 0;
    uint64_t frameSerial = 0;
    uint32_t obsReaderCount = 0;
    uint32_t obsSubmittedReaderCount = 0;
    uint32_t virtualCameraReaderCount = 0;
    bool gameWriterPending = false;
    bool recording = false;
    bool published = false;
};

struct VulkanVirtualCameraSlot {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    uint8_t* hostPointer = nullptr;
    VkDeviceSize capacity = 0;
    uint64_t generation = 0;
};

struct CursorTrailPoint {
    ImVec2 position{};
    int64_t timeMs = 0;
    float sizeBoost = 1.0f;
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
    // OBS has a persistent, passive context.  It shares the interactive
    // context's font atlas, but never owns a platform/renderer backend or
    // receives input.  This lets the OBS-only pass retain its visibility
    // filtering without advancing the interactive ImGui frame a second time.
    ImGuiContext* obsImGuiContext = nullptr;
    bool imguiWin32Initialized = false;
    bool imguiVulkanInitialized = false;
    ImDrawData settingsGuiDrawData{};
    ImDrawList* settingsGuiDrawList = nullptr;
    HWND hwnd = NULL;
    std::chrono::steady_clock::time_point lastSubclassCheck{};
    VkSampler mirrorSampler = VK_NULL_HANDLE;
    VkSampler linearSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout mirrorDescriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout mirrorPipelineLayout = VK_NULL_HANDLE;
    VkShaderModule mirrorVertexShader = VK_NULL_HANDLE;
    VkShaderModule mirrorFragmentShader = VK_NULL_HANDLE;
    VkShaderModule overlayFragmentShader = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkPipeline> mirrorPipelines;
    std::unordered_map<uint64_t, VkPipeline> overlayPipelines;
    PFN_vkCreateShaderModule createShaderModule = nullptr;
    PFN_vkDestroyShaderModule destroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout createDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout createPipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout destroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines createGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkCmdBindPipeline cmdBindPipeline = nullptr;
    PFN_vkCmdPushConstants cmdPushConstants = nullptr;
    PFN_vkCreateComputePipelines createComputePipelines = nullptr;
    PFN_vkCmdBindDescriptorSets cmdBindDescriptorSets = nullptr;
    PFN_vkCmdDispatch cmdDispatch = nullptr;
    PFN_vkAllocateDescriptorSets allocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets updateDescriptorSets = nullptr;
    PFN_vkGetMemoryHostPointerPropertiesEXT
        getMemoryHostPointerPropertiesEXT = nullptr;
    PFN_vkCmdCopyImage cmdCopyImage = nullptr;
    PFN_vkCmdCopyImageToBuffer cmdCopyImageToBuffer = nullptr;
    VkBuffer pickerReadbackBuffer = VK_NULL_HANDLE;
    VkDeviceMemory pickerReadbackMemory = VK_NULL_HANDLE;
    uint8_t* pickerReadbackMapped = nullptr;
    VkBuffer screenshotReadbackBuffer = VK_NULL_HANDLE;
    VkDeviceMemory screenshotReadbackMemory = VK_NULL_HANDLE;
    uint8_t* screenshotReadbackMapped = nullptr;
    VkDeviceSize screenshotReadbackCapacity = 0;
    VkShaderModule virtualCameraComputeShader = VK_NULL_HANDLE;
    VkDescriptorSetLayout virtualCameraDescriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout virtualCameraPipelineLayout = VK_NULL_HANDLE;
    VkPipeline virtualCameraPipeline = VK_NULL_HANDLE;
    VkDescriptorPool virtualCameraDescriptorPool = VK_NULL_HANDLE;
    std::array<VulkanVirtualCameraSlot, 3> virtualCameraSlots{};
    uint64_t virtualCameraRecordedCount = 0;
    uint64_t virtualCameraPublishedCount = 0;
    uint64_t virtualCameraDroppedCount = 0;
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
    std::vector<RetiredFontResource> retiredFontResources;
    VkQueryPool queryPool = VK_NULL_HANDLE;
    VkQueryPool mirrorQueryPool = VK_NULL_HANDLE;
    PFN_vkCmdBeginQuery cmdBeginQuery = nullptr;
    PFN_vkCmdEndQuery cmdEndQuery = nullptr;
    std::unordered_map<uint64_t, bool> mirrorHasContent;
    std::shared_ptr<const NinjabrainData> frameNinjabrainData;
    float timestampPeriodNs = 1.0f;
    std::array<TimestampFrame, kMaxTimestampFrames> timestamps{};
    uint32_t nextTimestamp = 0;
    uint32_t activeTimestampFrames = 1;

    std::unordered_map<VkImage, SampledImage> imageResources;
    std::unordered_map<VkImage, MirrorCopyImage> mirrorCopyImages;
    std::unordered_map<int, MirrorSnapshotState> mirrorSnapshots;
    std::vector<RetiredMirrorSnapshot> retiredMirrorSnapshots;
    std::unordered_map<std::string, TextureAsset> textureAssets;
    // The interactive pass owns logical frame selection. The passive OBS pass
    // reuses these exact choices so animated assets and throttled mirrors cannot
    // advance or cross a frame boundary independently.
    std::unordered_map<const TextureAsset*, TextureFrame*>
        frameResolvedTextureFrames;
    std::unordered_map<std::string, SampledImage*> frameResolvedMirrorSamples;
    std::vector<RetiredTextureAsset> retiredTextureAssets;
    std::unordered_map<std::string, StreamingTexture> streamingTextures;
    std::array<ObsCompositionSlot, kObsCompositionSlotCount> obsCompositionSlots{};
    uint32_t nextObsCompositionSlot = 0;
    uint32_t publishedObsCompositionSlot = UINT32_MAX;
    uint64_t nextObsCompositionSerial = 1;
    std::chrono::steady_clock::time_point lastObsCompositionUpdate{};
    uint64_t obsCompositionThrottleCount = 0;
    std::array<CursorTrailPoint, 512> cursorTrailPoints{};
    size_t cursorTrailStart = 0;
    size_t cursorTrailCount = 0;
    ImVec2 cursorTrailLastPosition{};
    bool cursorTrailHasLastPosition = false;
    std::array<ImVec2, 3> cursorTrailSamples{};
    size_t cursorTrailSampleCount = 0;
    int64_t cursorTrailLastCallMs = 0;
    HCURSOR cursorBitmapHandle = nullptr;
    CursorTextures::CursorData cursorBitmap;
    uint64_t cursorBitmapGeneration = 0;
    bool rebindIndicatorPreviousEnabled = false;
    std::chrono::steady_clock::time_point rebindIndicatorToggleTime{};
    float rebindIndicatorAlpha = 0.0f;
    std::vector<MirrorFragmentPushConstants> mirrorFragmentPushData;
    std::shared_ptr<const Config> configSnapshot;
    uint64_t configVersion = 0;
    std::string publishedModeId;
    std::string modeId;
    std::string pendingSourceModeId;
    std::chrono::steady_clock::time_point pendingSourceModeSince{};
    int modeCacheSourceWidth = 0;
    int modeCacheSourceHeight = 0;
    int modeCacheScreenWidth = 0;
    int modeCacheScreenHeight = 0;
    bool modeCacheImagesVisible = true;
    bool modeCacheWindowOverlaysVisible = true;
    bool modeCacheBrowserOverlaysVisible = true;
    std::vector<MirrorConfig> mirrors;
    std::vector<MirrorConfig> slideOutMirrors;
    std::string slideOutFromModeId;
    std::vector<ImageConfig> images;
    std::vector<const WindowOverlayConfig*> windowOverlays;
    std::vector<const BrowserOverlayConfig*> browserOverlays;
    VkDevice rebuildDevice = VK_NULL_HANDLE;
    VkFormat rebuildFormat = VK_FORMAT_UNDEFINED;
    bool rebuildPending = false;
    bool initialized = false;
};

RendererState g_state;
// RecordAfterFinalBlit mutates the renderer, ImGui contexts, and Vulkan
// resource maps as one transaction.  Destruction hooks may arrive on another
// thread, so serialize all state-mutating lifecycle transitions with recording.
// Recursive ownership permits the controlled rebuild path to call Shutdown
// without exposing a partially-destroyed state.
std::recursive_mutex g_rendererLifecycleMutex;
std::atomic<bool> g_ready{ false };
std::atomic<bool> g_deviceBeingDestroyed{ false };
std::atomic<bool> g_loggedInitializeFailure{ false };
std::atomic<bool> g_loggedBackendConflict{ false };
std::atomic<bool> g_loggedRendererEntry{ false };
std::atomic<bool> g_loggedFirstNativeOverlayFrame{ false };
std::atomic<bool> g_loggedNativeObsCapture{ false };
std::atomic<uint64_t> g_lastNativeOverlayCommandBuffer{ 0 };
std::atomic<uint64_t> g_lastNativeOverlayDestinationImage{ 0 };
std::atomic<uint64_t> g_lastNativeOverlaySerial{ 0 };
std::atomic<uint64_t> g_lastObsSubmitLogTick{ 0 };
std::atomic<uint64_t> g_lastObsPresentLogTick{ 0 };
std::atomic<uint64_t> g_obsCompositionFrameSerial{ 0 };
std::atomic<uint64_t> g_obsRedirectSubmittedCount{ 0 };
std::atomic<uint64_t> g_obsRedirectRetiredCount{ 0 };
std::atomic<uint64_t> g_obsRedirectFallbackCount{ 0 };
std::atomic<uint64_t> g_lastObsCompositionLogTick{ 0 };
std::atomic<uint64_t> g_lastObsPublishLogTick{ 0 };
std::atomic<uint64_t> g_lastGameGuiLifecycleLogTick{ 0 };
std::atomic<uint64_t> g_lastObsGuiLifecycleLogTick{ 0 };
std::atomic<bool> g_obsExportReady{ false };
std::mutex g_obsCompositionMutex;
thread_local bool g_obsCompositionPass = false;
thread_local VkCommandBuffer g_activeImGuiCommandBuffer = VK_NULL_HANDLE;
thread_local const VulkanRenderer::FinalBlitContext* g_activeFrameContext =
    nullptr;
thread_local TimestampFrame* g_activeResourceTimestampFrame = nullptr;

bool ShouldLogObsCapture(std::atomic<uint64_t>& lastTick) {
    constexpr uint64_t intervalMs = 2000;
    const uint64_t now = GetTickCount64();
    uint64_t previous = lastTick.load(std::memory_order_relaxed);
    while (now - previous >= intervalMs || previous == 0) {
        if (lastTick.compare_exchange_weak(previous, now, std::memory_order_relaxed)) return true;
    }
    return false;
}

void LogGuiLifecycle(const char* pass, ImGuiContext* context, bool consumedInput) {
    std::atomic<uint64_t>& lastTick = consumedInput
        ? g_lastGameGuiLifecycleLogTick
        : g_lastObsGuiLifecycleLogTick;
    if (!g_showGui.load(std::memory_order_acquire) ||
        !ShouldLogObsCapture(lastTick)) return;
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiContext& current = *ImGui::GetCurrentContext();
    Log("[VULKAN][GUI] pass=" + std::string(pass) +
        " context=" + std::to_string(reinterpret_cast<uintptr_t>(context)) +
        " frame=" + std::to_string(current.FrameCount) +
        " hoveredId=" + std::to_string(current.HoveredId) +
        " activeId=" + std::to_string(current.ActiveId) +
        " mouse=" + std::to_string(static_cast<int>(io.MousePos.x)) + "," +
        std::to_string(static_cast<int>(io.MousePos.y)) +
        " buttons=" + std::to_string(io.MouseDown[0]) + "," +
        std::to_string(io.MouseDown[1]) +
        " guiOpen=1 inputConsumed=" + std::to_string(consumedInput) +
        " captureMouse=" + std::to_string(io.WantCaptureMouse) +
        " captureKeyboard=" + std::to_string(io.WantCaptureKeyboard) + ".");
}

// OBS's Vulkan graphics hook owns the capture/export operation.  It intercepts
// vkQueuePresentKHR and copies the completed swapchain image into its shared
// texture.  Toolscreen therefore must not create a second export image or use
// the legacy OpenGL override; recording before Minecraft queues this command
// buffer is the complete native integration point.
void ObserveNativeObsCapture() {
    if (!GetModuleHandleA("graphics-hook64.dll")) return;
    if (!g_loggedNativeObsCapture.exchange(true, std::memory_order_acq_rel)) {
        Log("[VULKAN][OBS] OBS graphics-hook64.dll detected. The upper OBS Vulkan hook remains untouched; the "
            "process-local lower layer will redirect only OBS's validated export copy to a completed native "
            "Toolscreen composition. The real Minecraft swapchain image is still presented, with no GL redirect, "
            "shared GL texture, or CPU capture frame.");
    }
}

void LogInitializeFailure(std::string_view reason) {
    if (!g_loggedInitializeFailure.exchange(true, std::memory_order_acq_rel)) {
        Log("[VULKAN] Native renderer initialization is waiting: " + std::string(reason) + ".");
    }
}

VkImageMemoryBarrier MakeBarrier(VkImage image, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess,
                                 VkImageLayout oldLayout, VkImageLayout newLayout);
uint32_t PendingTimestampFrameMask();

void CheckVkResult(VkResult result) {
    if (result < 0) {
        Log("[VULKAN] ImGui backend error: " + std::to_string(static_cast<int>(result)));
    }
}

bool IsTrackedMinecraftWindow(HWND hwnd) {
    DWORD pid = 0;
    return hwnd && IsWindow(hwnd) &&
           GetWindowThreadProcessId(hwnd, &pid) != 0 &&
           pid == GetCurrentProcessId();
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

bool IsScreenshotFormatSupported(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return true;
    default:
        return false;
    }
}

bool EnsureScreenshotReadbackResources(VkDeviceSize requiredSize) {
    if (requiredSize == 0) return false;
    if (g_state.screenshotReadbackBuffer &&
        g_state.screenshotReadbackCapacity >= requiredSize) {
        return true;
    }
    if (g_state.screenshotReadbackBuffer && std::any_of(
            g_state.timestamps.begin(), g_state.timestamps.end(),
            [](const TimestampFrame& frame) { return frame.pending; })) {
        return false;
    }

    if (g_state.screenshotReadbackMapped && g_state.dispatch.unmapMemory)
        g_state.dispatch.unmapMemory(
            g_state.device, g_state.screenshotReadbackMemory);
    if (g_state.screenshotReadbackBuffer && g_state.dispatch.destroyBuffer)
        g_state.dispatch.destroyBuffer(
            g_state.device, g_state.screenshotReadbackBuffer, nullptr);
    if (g_state.screenshotReadbackMemory && g_state.dispatch.freeMemory)
        g_state.dispatch.freeMemory(
            g_state.device, g_state.screenshotReadbackMemory, nullptr);
    g_state.screenshotReadbackBuffer = VK_NULL_HANDLE;
    g_state.screenshotReadbackMemory = VK_NULL_HANDLE;
    g_state.screenshotReadbackMapped = nullptr;
    g_state.screenshotReadbackCapacity = 0;

    if (!g_state.dispatch.createBuffer ||
        !g_state.dispatch.getBufferMemoryRequirements ||
        !g_state.dispatch.allocateMemory ||
        !g_state.dispatch.bindBufferMemory ||
        !g_state.dispatch.mapMemory) {
        return false;
    }
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = requiredSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_state.dispatch.createBuffer(
            g_state.device, &bufferInfo, nullptr,
            &g_state.screenshotReadbackBuffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements{};
    g_state.dispatch.getBufferMemoryRequirements(
        g_state.device, g_state.screenshotReadbackBuffer, &requirements);
    uint32_t memoryType = 0;
    if (!FindMemoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            memoryType)) {
        g_state.dispatch.destroyBuffer(
            g_state.device, g_state.screenshotReadbackBuffer, nullptr);
        g_state.screenshotReadbackBuffer = VK_NULL_HANDLE;
        return false;
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (g_state.dispatch.allocateMemory(
            g_state.device, &allocation, nullptr,
            &g_state.screenshotReadbackMemory) != VK_SUCCESS ||
        g_state.dispatch.bindBufferMemory(
            g_state.device, g_state.screenshotReadbackBuffer,
            g_state.screenshotReadbackMemory, 0) != VK_SUCCESS ||
        g_state.dispatch.mapMemory(
            g_state.device, g_state.screenshotReadbackMemory, 0,
            requirements.size, 0,
            reinterpret_cast<void**>(
                &g_state.screenshotReadbackMapped)) != VK_SUCCESS) {
        if (g_state.screenshotReadbackMemory)
            g_state.dispatch.freeMemory(
                g_state.device, g_state.screenshotReadbackMemory, nullptr);
        if (g_state.screenshotReadbackBuffer)
            g_state.dispatch.destroyBuffer(
                g_state.device, g_state.screenshotReadbackBuffer, nullptr);
        g_state.screenshotReadbackBuffer = VK_NULL_HANDLE;
        g_state.screenshotReadbackMemory = VK_NULL_HANDLE;
        g_state.screenshotReadbackMapped = nullptr;
        return false;
    }
    g_state.screenshotReadbackCapacity = requirements.size;
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
    g_state.cmdPushConstants = reinterpret_cast<PFN_vkCmdPushConstants>(load("vkCmdPushConstants"));
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
    fragmentInfo.codeSize = sizeof(kOverlayFragmentShader);
    fragmentInfo.pCode = kOverlayFragmentShader;
    if (g_state.createShaderModule(
            g_state.device, &fragmentInfo, nullptr,
            &g_state.overlayFragmentShader) != VK_SUCCESS) {
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

    std::array<VkPushConstantRange, 2> pushConstants{};
    pushConstants[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstants[0].size = sizeof(float) * 4;
    pushConstants[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstants[1].offset = sizeof(float) * 4;
    pushConstants[1].size = sizeof(MirrorFragmentPushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &g_state.mirrorDescriptorSetLayout;
    layoutInfo.pushConstantRangeCount = static_cast<uint32_t>(pushConstants.size());
    layoutInfo.pPushConstantRanges = pushConstants.data();
    return g_state.createPipelineLayout(
               g_state.device, &layoutInfo, nullptr, &g_state.mirrorPipelineLayout) == VK_SUCCESS;
}

void DestroyVirtualCameraSlot(VulkanVirtualCameraSlot& slot) {
    if (slot.buffer && g_state.dispatch.destroyBuffer) {
        g_state.dispatch.destroyBuffer(g_state.device, slot.buffer, nullptr);
    }
    if (slot.memory && g_state.dispatch.freeMemory) {
        g_state.dispatch.freeMemory(g_state.device, slot.memory, nullptr);
    }
    const VkDescriptorSet descriptorSet = slot.descriptorSet;
    slot = {};
    slot.descriptorSet = descriptorSet;
}

bool CreateVirtualCameraPipelineResources() {
    auto load = [](const char* name) {
        return VulkanHooks::LoadRealFunction(
            name, reinterpret_cast<void*>(g_state.device));
    };
    auto getQueueProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            load("vkGetPhysicalDeviceQueueFamilyProperties"));
    uint32_t queueFamilyCount = 0;
    if (!getQueueProperties) return false;
    getQueueProperties(
        g_state.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    getQueueProperties(
        g_state.physicalDevice, &queueFamilyCount, queueFamilies.data());
    if (g_state.queueFamily >= queueFamilies.size() ||
        (queueFamilies[g_state.queueFamily].queueFlags &
         VK_QUEUE_COMPUTE_BIT) == 0) {
        Log("[VULKAN][VIRTUALCAM] Minecraft's presentation queue does not "
            "support compute; direct NV12 output is unavailable without an "
            "extra synchronized submission.");
        return false;
    }
    g_state.createComputePipelines =
        reinterpret_cast<PFN_vkCreateComputePipelines>(
            load("vkCreateComputePipelines"));
    g_state.cmdBindDescriptorSets =
        reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
            load("vkCmdBindDescriptorSets"));
    g_state.cmdDispatch =
        reinterpret_cast<PFN_vkCmdDispatch>(load("vkCmdDispatch"));
    g_state.allocateDescriptorSets =
        reinterpret_cast<PFN_vkAllocateDescriptorSets>(
            load("vkAllocateDescriptorSets"));
    g_state.updateDescriptorSets =
        reinterpret_cast<PFN_vkUpdateDescriptorSets>(
            load("vkUpdateDescriptorSets"));
    g_state.getMemoryHostPointerPropertiesEXT =
        reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
            load("vkGetMemoryHostPointerPropertiesEXT"));
    if (!g_state.createComputePipelines || !g_state.cmdBindDescriptorSets ||
        !g_state.cmdDispatch || !g_state.allocateDescriptorSets ||
        !g_state.updateDescriptorSets ||
        !g_state.getMemoryHostPointerPropertiesEXT ||
        !g_state.dispatch.createDescriptorPool ||
        !g_state.dispatch.destroyDescriptorPool) {
        Log("[VULKAN][VIRTUALCAM] Direct shared-memory import is unavailable; "
            "the Vulkan path remains disabled without falling back to a CPU "
            "full-frame readback.");
        return false;
    }

    VkShaderModuleCreateInfo shaderInfo{
        VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    shaderInfo.codeSize = sizeof(kVirtualCameraComputeShader);
    shaderInfo.pCode = kVirtualCameraComputeShader;
    if (g_state.createShaderModule(
            g_state.device, &shaderInfo, nullptr,
            &g_state.virtualCameraComputeShader) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptorLayoutInfo.bindingCount =
        static_cast<uint32_t>(bindings.size());
    descriptorLayoutInfo.pBindings = bindings.data();
    if (g_state.createDescriptorSetLayout(
            g_state.device, &descriptorLayoutInfo, nullptr,
            &g_state.virtualCameraDescriptorSetLayout) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushRange.size = sizeof(uint32_t) * 6u;
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts =
        &g_state.virtualCameraDescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (g_state.createPipelineLayout(
            g_state.device, &pipelineLayoutInfo, nullptr,
            &g_state.virtualCameraPipelineLayout) != VK_SUCCESS) {
        return false;
    }

    VkPipelineShaderStageCreateInfo stage{
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = g_state.virtualCameraComputeShader;
    stage.pName = "main";
    VkComputePipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = stage;
    pipelineInfo.layout = g_state.virtualCameraPipelineLayout;
    if (g_state.createComputePipelines(
            g_state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
            &g_state.virtualCameraPipeline) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo poolInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 3;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    if (g_state.dispatch.createDescriptorPool(
            g_state.device, &poolInfo, nullptr,
            &g_state.virtualCameraDescriptorPool) != VK_SUCCESS) {
        return false;
    }
    std::array<VkDescriptorSetLayout, 3> layouts{
        g_state.virtualCameraDescriptorSetLayout,
        g_state.virtualCameraDescriptorSetLayout,
        g_state.virtualCameraDescriptorSetLayout};
    std::array<VkDescriptorSet, 3> sets{};
    VkDescriptorSetAllocateInfo allocateInfo{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = g_state.virtualCameraDescriptorPool;
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(sets.size());
    allocateInfo.pSetLayouts = layouts.data();
    if (g_state.allocateDescriptorSets(
            g_state.device, &allocateInfo, sets.data()) != VK_SUCCESS) {
        return false;
    }
    for (size_t index = 0; index < sets.size(); ++index) {
        g_state.virtualCameraSlots[index].descriptorSet = sets[index];
    }
    Log("[VULKAN][VIRTUALCAM] Native compute conversion initialized; NV12 "
        "frames will be written directly into imported OBS virtual-camera "
        "shared memory without CPU readback.");
    return true;
}

bool EnsureVirtualCameraSlot(
    const VirtualCameraGpuFrame& frame, VulkanVirtualCameraSlot& slot) {
    if (slot.buffer && slot.hostPointer == frame.data &&
        slot.capacity >= frame.capacityBytes &&
        slot.generation == frame.generation) {
        return true;
    }
    DestroyVirtualCameraSlot(slot);
    if (!frame.data || frame.capacityBytes == 0 ||
        !g_state.getMemoryHostPointerPropertiesEXT) {
        return false;
    }

    VkExternalMemoryBufferCreateInfo externalInfo{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    externalInfo.handleTypes =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.pNext = &externalInfo;
    bufferInfo.size = frame.capacityBytes;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_state.dispatch.createBuffer(
            g_state.device, &bufferInfo, nullptr, &slot.buffer) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    g_state.dispatch.getBufferMemoryRequirements(
        g_state.device, slot.buffer, &requirements);
    VkMemoryHostPointerPropertiesEXT hostProperties{
        VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (requirements.size > frame.capacityBytes ||
        g_state.getMemoryHostPointerPropertiesEXT(
            g_state.device,
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            frame.data, &hostProperties) != VK_SUCCESS) {
        DestroyVirtualCameraSlot(slot);
        return false;
    }
    const uint32_t candidates =
        requirements.memoryTypeBits & hostProperties.memoryTypeBits;
    uint32_t memoryTypeIndex = UINT32_MAX;
    // The OBS consumer reads the imported pointer directly after the GPU
    // completion query retires. Restrict the import to coherent host-visible
    // memory so publication never requires mapping/invalidation or a CPU copy.
    if (!FindMemoryType(
            candidates,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            memoryTypeIndex)) {
        DestroyVirtualCameraSlot(slot);
        return false;
    }

    VkImportMemoryHostPointerInfoEXT importInfo{
        VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    importInfo.handleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    importInfo.pHostPointer = frame.data;
    VkMemoryAllocateInfo allocation{
        VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.pNext = &importInfo;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryTypeIndex;
    if (g_state.dispatch.allocateMemory(
            g_state.device, &allocation, nullptr, &slot.memory) != VK_SUCCESS ||
        g_state.dispatch.bindBufferMemory(
            g_state.device, slot.buffer, slot.memory, 0) != VK_SUCCESS) {
        DestroyVirtualCameraSlot(slot);
        return false;
    }
    slot.hostPointer = frame.data;
    slot.capacity = frame.capacityBytes;
    slot.generation = frame.generation;
    return true;
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
    static_assert(sizeof(MirrorSpecialization) == sizeof(uint32_t) * 90);
    const uint64_t key = HashMirrorSpecialization(specialization);
    if (const auto it = g_state.mirrorPipelines.find(key); it != g_state.mirrorPipelines.end()) {
        return it->second;
    }

    std::array<VkSpecializationMapEntry, 90> entries{};
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

VkPipeline GetOverlayPipeline(
    const OverlaySpecialization& specialization,
    OverlayBlendMode blendMode = OverlayBlendMode::Alpha) {
    static_assert(sizeof(OverlaySpecialization) == sizeof(uint32_t) * 33);
    uint64_t overlayKey = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&specialization);
    for (size_t i = 0; i < sizeof(specialization); ++i) {
        overlayKey ^= bytes[i];
        overlayKey *= 1099511628211ull;
    }
    overlayKey ^= static_cast<uint64_t>(blendMode);
    overlayKey *= 1099511628211ull;
    if (const auto it = g_state.overlayPipelines.find(overlayKey);
        it != g_state.overlayPipelines.end()) {
        return it->second;
    }

    std::array<VkSpecializationMapEntry, 33> entries{};
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
    stages[1].module = g_state.overlayFragmentShader;
    stages[1].pName = "main";
    stages[1].pSpecializationInfo = &specializationInfo;

    VkVertexInputBindingDescription vertexBinding{};
    vertexBinding.binding = 0;
    vertexBinding.stride = sizeof(ImDrawVert);
    vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::array<VkVertexInputAttributeDescription, 3> attributes{};
    attributes[0] = {
        0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, pos) };
    attributes[1] = {
        1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(ImDrawVert, uv) };
    attributes[2] = {
        2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(ImDrawVert, col) };
    VkPipelineVertexInputStateCreateInfo vertexInput{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &vertexBinding;
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = VK_TRUE;
    const bool additive = blendMode == OverlayBlendMode::Additive;
    const bool invert = blendMode == OverlayBlendMode::Invert;
    attachment.srcColorBlendFactor = invert
        ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR
        : VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = additive
        ? VK_BLEND_FACTOR_ONE
        : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = invert
        ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA
        : (additive ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE);
    attachment.dstAlphaBlendFactor = additive
        ? VK_BLEND_FACTOR_ONE
        : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    const VkDynamicState dynamicStates[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &g_state.format;
    VkGraphicsPipelineCreateInfo pipelineInfo{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
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
            g_state.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
            &pipeline) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    g_state.overlayPipelines.emplace(overlayKey, pipeline);
    return pipeline;
}

void BindMirrorPipelineCallback(const ImDrawList*, const ImDrawCmd* command) {
    if (!g_activeImGuiCommandBuffer || !g_state.cmdBindPipeline) return;
    const VkPipeline pipeline = reinterpret_cast<VkPipeline>(command->UserCallbackData);
    if (pipeline) {
        g_state.cmdBindPipeline(g_activeImGuiCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    }
}

void PushMirrorGradientTimeCallback(const ImDrawList*, const ImDrawCmd* command) {
    if (!g_activeImGuiCommandBuffer || !g_state.cmdPushConstants ||
        !g_state.mirrorPipelineLayout) {
        return;
    }
    const uintptr_t encoded =
        reinterpret_cast<uintptr_t>(command->UserCallbackData);
    MirrorFragmentPushConstants push{};
    push.gradientTime =
        static_cast<float>(encoded > 0 ? encoded - 1u : 0u) / 1000.0f;
    g_state.cmdPushConstants(
        g_activeImGuiCommandBuffer, g_state.mirrorPipelineLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(float) * 4, sizeof(push),
        &push);
}

void PushMirrorFragmentDataCallback(const ImDrawList*, const ImDrawCmd* command) {
    if (!g_activeImGuiCommandBuffer || !g_state.cmdPushConstants ||
        !g_state.mirrorPipelineLayout || !command->UserCallbackData) {
        return;
    }
    const auto* push = static_cast<const MirrorFragmentPushConstants*>(
        command->UserCallbackData);
    g_state.cmdPushConstants(
        g_activeImGuiCommandBuffer, g_state.mirrorPipelineLayout,
        VK_SHADER_STAGE_FRAGMENT_BIT, sizeof(float) * 4, sizeof(*push), push);
}

VkPipeline GetColorKeyPipeline(
    bool enabled, const std::vector<ColorKeyConfig>& colorKeys) {
    if (!enabled || colorKeys.empty()) return VK_NULL_HANDLE;
    OverlaySpecialization specialization{};
    specialization.keyCount = static_cast<int32_t>(
        (std::min)(colorKeys.size(), size_t{ 8 }));
    for (int32_t i = 0; i < specialization.keyCount; ++i) {
        const ColorKeyConfig& key = colorKeys[static_cast<size_t>(i)];
        specialization.keys[i][0] = key.color.r;
        specialization.keys[i][1] = key.color.g;
        specialization.keys[i][2] = key.color.b;
        specialization.sensitivities[i] = key.sensitivity;
    }
    return GetOverlayPipeline(specialization);
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
    if (g_state.dispatch.createImageView(g_state.device, &info, nullptr, &result.view) != VK_SUCCESS) {
        return false;
    }
    result.image = image;
    return true;
}

bool IsFormatSampleable(VkFormat format) {
    auto getFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        VulkanHooks::LoadRealFunction("vkGetPhysicalDeviceFormatProperties",
                                      reinterpret_cast<void*>(g_state.device)));
    if (!getFormatProperties || format == VK_FORMAT_UNDEFINED) return false;
    VkFormatProperties properties{};
    getFormatProperties(g_state.physicalDevice, format, &properties);
    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

bool IsFormatTransferDestination(VkFormat format) {
    auto getFormatProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
        VulkanHooks::LoadRealFunction("vkGetPhysicalDeviceFormatProperties",
                                      reinterpret_cast<void*>(g_state.device)));
    if (!getFormatProperties || format == VK_FORMAT_UNDEFINED) return false;
    VkFormatProperties properties{};
    getFormatProperties(g_state.physicalDevice, format, &properties);
    return (properties.optimalTilingFeatures &
            VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0;
}

void DestroyMirrorCopyImage(MirrorCopyImage& resource) {
    ImGui::SetCurrentContext(g_state.imguiContext);
    if (resource.sampled.descriptor) {
        ImGui_ImplVulkan_RemoveTexture(resource.sampled.descriptor);
    }
    if (resource.sampled.linearDescriptor) {
        ImGui_ImplVulkan_RemoveTexture(resource.sampled.linearDescriptor);
    }
    if (resource.sampled.view && g_state.dispatch.destroyImageView) {
        g_state.dispatch.destroyImageView(g_state.device, resource.sampled.view, nullptr);
    }
    if (resource.sampled.image && g_state.dispatch.destroyImage) {
        g_state.dispatch.destroyImage(g_state.device, resource.sampled.image, nullptr);
    }
    if (resource.memory && g_state.dispatch.freeMemory) {
        g_state.dispatch.freeMemory(g_state.device, resource.memory, nullptr);
    }
    resource = {};
}

bool MirrorCopyMatches(const MirrorCopyImage& resource, const ImageMetadata& metadata) {
    return resource.sampled.image != VK_NULL_HANDLE &&
           resource.format == metadata.format &&
           resource.extent.width == metadata.extent.width &&
           resource.extent.height == metadata.extent.height &&
           resource.extent.depth == metadata.extent.depth &&
           resource.samples == metadata.samples;
}

bool CreateMirrorCopyImage(const ImageMetadata& metadata, MirrorCopyImage& resource) {
    if (!g_state.dispatch.createImage || !g_state.dispatch.getImageMemoryRequirements ||
        !g_state.dispatch.allocateMemory || !g_state.dispatch.bindImageMemory ||
        !IsFormatSampleable(metadata.format) ||
        !IsFormatTransferDestination(metadata.format) ||
        metadata.type != VK_IMAGE_TYPE_2D || metadata.extent.width == 0 ||
        metadata.extent.height == 0) {
        return false;
    }

    VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = metadata.format;
    imageInfo.extent = metadata.extent;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_state.dispatch.createImage(
            g_state.device, &imageInfo, nullptr, &resource.sampled.image) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements{};
    g_state.dispatch.getImageMemoryRequirements(
        g_state.device, resource.sampled.image, &requirements);
    uint32_t memoryType = 0;
    if (!FindMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memoryType)) {
        DestroyMirrorCopyImage(resource);
        return false;
    }
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (g_state.dispatch.allocateMemory(
            g_state.device, &allocation, nullptr, &resource.memory) != VK_SUCCESS ||
        g_state.dispatch.bindImageMemory(
            g_state.device, resource.sampled.image, resource.memory, 0) != VK_SUCCESS) {
        DestroyMirrorCopyImage(resource);
        return false;
    }

    ImageMetadata ownedMetadata = metadata;
    ownedMetadata.usage = imageInfo.usage;
    ownedMetadata.tiling = VK_IMAGE_TILING_OPTIMAL;
    ownedMetadata.samples = VK_SAMPLE_COUNT_1_BIT;
    if (!CreateImageView(resource.sampled.image, ownedMetadata, resource.sampled)) {
        DestroyMirrorCopyImage(resource);
        return false;
    }
    resource.sampled.descriptor = ImGui_ImplVulkan_AddTexture(
        g_state.mirrorSampler, resource.sampled.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!resource.sampled.descriptor) {
        DestroyMirrorCopyImage(resource);
        return false;
    }
    resource.sampled.descriptorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    resource.format = metadata.format;
    resource.extent = metadata.extent;
    resource.samples = metadata.samples;
    return true;
}

bool CreateObsCompositionImage(
    const ImageMetadata& metadata, MirrorCopyImage& resource) {
    if (!g_state.dispatch.createImage ||
        !g_state.dispatch.getImageMemoryRequirements ||
        !g_state.dispatch.allocateMemory ||
        !g_state.dispatch.bindImageMemory ||
        metadata.format == VK_FORMAT_UNDEFINED ||
        metadata.extent.width == 0 || metadata.extent.height == 0 ||
        metadata.type != VK_IMAGE_TYPE_2D) {
        return false;
    }
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = metadata.format;
    imageInfo.extent = metadata.extent;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_state.dispatch.createImage(
            g_state.device, &imageInfo, nullptr,
            &resource.sampled.image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements{};
    g_state.dispatch.getImageMemoryRequirements(
        g_state.device, resource.sampled.image, &requirements);
    uint32_t memoryType = 0;
    if (!FindMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            memoryType)) {
        DestroyMirrorCopyImage(resource);
        return false;
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (g_state.dispatch.allocateMemory(
            g_state.device, &allocation, nullptr,
            &resource.memory) != VK_SUCCESS ||
        g_state.dispatch.bindImageMemory(
            g_state.device, resource.sampled.image, resource.memory,
            0) != VK_SUCCESS) {
        DestroyMirrorCopyImage(resource);
        return false;
    }
    ImageMetadata owned = metadata;
    owned.usage = imageInfo.usage;
    owned.samples = VK_SAMPLE_COUNT_1_BIT;
    owned.tiling = VK_IMAGE_TILING_OPTIMAL;
    if (!CreateImageView(
            resource.sampled.image, owned, resource.sampled)) {
        DestroyMirrorCopyImage(resource);
        return false;
    }
    resource.format = metadata.format;
    resource.extent = metadata.extent;
    resource.samples = VK_SAMPLE_COUNT_1_BIT;
    resource.initialized = false;
    return true;
}

void RetireMirrorCopyImage(MirrorCopyImage&& image) {
    if (!image.sampled.image) return;
    const uint32_t pendingFrameMask = PendingTimestampFrameMask();
    if (pendingFrameMask == 0) {
        DestroyMirrorCopyImage(image);
        return;
    }
    RetiredMirrorSnapshot retired{};
    retired.image = std::move(image);
    retired.pendingFrameMask = pendingFrameMask;
    g_state.retiredMirrorSnapshots.push_back(std::move(retired));
}

MirrorCopyImage* GetMirrorCopyImage(VkImage source, const ImageMetadata& metadata) {
    auto [it, inserted] = g_state.mirrorCopyImages.try_emplace(source);
    if (!inserted && !MirrorCopyMatches(it->second, metadata)) {
        // A recycled source-image handle can arrive while the old copy is
        // still referenced by one of Minecraft's submitted command buffers.
        // Retire that image using the real frame-completion mask instead of
        // replacing it in place.
        MirrorCopyImage old = std::move(it->second);
        RetireMirrorCopyImage(std::move(old));
    }
    if (!it->second.sampled.image && !CreateMirrorCopyImage(metadata, it->second)) {
        g_state.mirrorCopyImages.erase(it);
        return nullptr;
    }
    return &it->second;
}

void RetireMirrorSnapshotState(MirrorSnapshotState& state) {
    for (MirrorSnapshotSlot& slot : state.slots) {
        if (!slot.image.sampled.image) continue;
        if (slot.pendingFrameMask == 0) {
            DestroyMirrorCopyImage(slot.image);
        } else {
            RetiredMirrorSnapshot retired{};
            retired.image = std::move(slot.image);
            retired.pendingFrameMask = slot.pendingFrameMask;
            g_state.retiredMirrorSnapshots.push_back(std::move(retired));
        }
    }
    state = {};
}

uint32_t TimestampFrameBit(const TimestampFrame* frame) {
    if (!frame || !g_state.queryPool) return 0;
    const uint32_t index = frame->firstQuery / kQueriesPerFrame;
    return index < kMaxTimestampFrames ? (1u << index) : 0;
}

uint32_t PendingTimestampFrameMask() {
    uint32_t mask = 0;
    for (const TimestampFrame& frame : g_state.timestamps) {
        if (frame.pending) mask |= TimestampFrameBit(&frame);
    }
    return mask;
}

bool CanCopyPreparedMirrorSource(
    const VulkanRenderer::FinalBlitContext& context, const SampledImage* sampled) {
    if (!sampled || !sampled->image || !g_state.cmdCopyImage ||
        !context.sourceMetadata) {
        return false;
    }
    if (sampled->image != context.sourceImage) {
        // Toolscreen-owned fallback images include TRANSFER_SRC usage.
        return true;
    }
    return (context.sourceMetadata->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
}

bool UpdateMirrorSnapshot(
    const VulkanRenderer::FinalBlitContext& context, SampledImage* sampled,
    VkImageLayout sampleLayout, int fps, TimestampFrame* timestampFrame) {
    if (MirrorUsesEveryFrameUpdates(fps) || !timestampFrame ||
        !CanCopyPreparedMirrorSource(context, sampled)) {
        return false;
    }

    MirrorSnapshotState& state = g_state.mirrorSnapshots[fps];
    const size_t requiredSlots =
        static_cast<size_t>(g_state.activeTimestampFrames) + 1;
    if (state.slots.size() < requiredSlots) {
        state.slots.resize(requiredSlots);
    }

    if (state.latestSlot != std::numeric_limits<size_t>::max()) {
        const MirrorSnapshotSlot& latest = state.slots[state.latestSlot];
        if (!MirrorCopyMatches(latest.image, *context.sourceMetadata)) {
            RetireMirrorSnapshotState(state);
            state.slots.resize(requiredSlots);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    const bool initialized =
        state.latestSlot != std::numeric_limits<size_t>::max();
    const bool updateDue =
        !initialized ||
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - state.lastUpdate).count() >= (1000 / (std::max)(1, fps));
    if (!updateDue) return true;

    size_t writableSlot = std::numeric_limits<size_t>::max();
    for (size_t i = 0; i < state.slots.size(); ++i) {
        if (state.slots[i].pendingFrameMask == 0) {
            writableSlot = i;
            break;
        }
    }
    if (writableSlot == std::numeric_limits<size_t>::max()) {
        // Every slot is referenced by an actual in-flight Minecraft frame.
        // Retain the previous snapshot and try again next frame.
        return initialized;
    }

    MirrorSnapshotSlot& destination = state.slots[writableSlot];
    if (destination.image.sampled.image &&
        !MirrorCopyMatches(destination.image, *context.sourceMetadata)) {
        DestroyMirrorCopyImage(destination.image);
    }
    if (!destination.image.sampled.image &&
        !CreateMirrorCopyImage(*context.sourceMetadata, destination.image)) {
        return initialized;
    }

    const VkImageLayout preparedLayout =
        sampleLayout != VK_IMAGE_LAYOUT_UNDEFINED
            ? sampleLayout
            : sampled->descriptorLayout;
    VkImageMemoryBarrier sourceToTransfer = MakeBarrier(
        sampled->image, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        preparedLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageMemoryBarrier destinationToTransfer = MakeBarrier(
        destination.image.sampled.image,
        destination.image.initialized ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_NONE,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        destination.image.initialized
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageMemoryBarrier toTransfer[] = {
        sourceToTransfer, destinationToTransfer
    };
    g_state.dispatch.cmdPipelineBarrier(
        context.commandBuffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<uint32_t>(std::size(toTransfer)), toTransfer);

    VkImageCopy copy{};
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.dstSubresource.layerCount = 1;
    copy.extent = context.sourceMetadata->extent;
    g_state.cmdCopyImage(
        context.commandBuffer, sampled->image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        destination.image.sampled.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageMemoryBarrier sourceToShader = MakeBarrier(
        sampled->image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, preparedLayout);
    VkImageMemoryBarrier destinationToShader = MakeBarrier(
        destination.image.sampled.image, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    const VkImageMemoryBarrier toShader[] = {
        sourceToShader, destinationToShader
    };
    g_state.dispatch.cmdPipelineBarrier(
        context.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<uint32_t>(std::size(toShader)), toShader);

    destination.image.initialized = true;
    state.latestSlot = writableSlot;
    state.lastUpdate = now;
    return true;
}

void PrepareMirrorSnapshots(
    const VulkanRenderer::FinalBlitContext& context, SampledImage* sampled,
    VkImageLayout sampleLayout, TimestampFrame* timestampFrame) {
    if (!sampled || !timestampFrame) return;
    std::array<int, kMaxMirrorQueriesPerFrame> preparedFps{};
    size_t preparedCount = 0;
    for (const MirrorConfig& mirror : g_state.mirrors) {
        if (MirrorUsesEveryFrameUpdates(mirror.fps)) continue;
        bool alreadyPrepared = false;
        for (size_t i = 0; i < preparedCount; ++i) {
            if (preparedFps[i] == mirror.fps) {
                alreadyPrepared = true;
                break;
            }
        }
        if (alreadyPrepared) continue;
        UpdateMirrorSnapshot(
            context, sampled, sampleLayout, mirror.fps, timestampFrame);
        if (preparedCount < preparedFps.size()) {
            preparedFps[preparedCount++] = mirror.fps;
        }
    }
}

SampledImage* PrepareEyeZoomSource(
    const VulkanRenderer::FinalBlitContext& context, SampledImage* realtime,
    VkImageLayout sampleLayout, TimestampFrame* timestampFrame) {
    const bool showEyeZoom =
        g_showEyeZoom.load(std::memory_order_acquire);
    const bool transitioningFromEyeZoom =
        g_isTransitioningFromEyeZoom.load(std::memory_order_acquire);
    auto snapshotIt =
        g_state.mirrorSnapshots.find(kEyeZoomSnapshotFpsKey);

    if (!showEyeZoom && !transitioningFromEyeZoom) {
        if (snapshotIt != g_state.mirrorSnapshots.end()) {
            RetireMirrorSnapshotState(snapshotIt->second);
            g_state.mirrorSnapshots.erase(snapshotIt);
        }
        return realtime;
    }

    if (!transitioningFromEyeZoom && realtime && timestampFrame) {
        UpdateMirrorSnapshot(
            context, realtime, sampleLayout, kEyeZoomSnapshotFpsKey,
            timestampFrame);
        snapshotIt =
            g_state.mirrorSnapshots.find(kEyeZoomSnapshotFpsKey);
        if (snapshotIt != g_state.mirrorSnapshots.end() &&
            snapshotIt->second.latestSlot !=
                std::numeric_limits<size_t>::max()) {
            snapshotIt->second
                .slots[snapshotIt->second.latestSlot]
                .pendingFrameMask |= TimestampFrameBit(timestampFrame);
        }
        return realtime;
    }

    if (snapshotIt == g_state.mirrorSnapshots.end() ||
        snapshotIt->second.latestSlot ==
            std::numeric_limits<size_t>::max() ||
        !timestampFrame) {
        return realtime;
    }
    MirrorSnapshotSlot& snapshot =
        snapshotIt->second.slots[snapshotIt->second.latestSlot];
    if (!snapshot.image.initialized) return realtime;
    snapshot.pendingFrameMask |= TimestampFrameBit(timestampFrame);
    return &snapshot.image.sampled;
}

SampledImage* ResolveMirrorSample(
    const MirrorConfig& mirror, SampledImage* realtime,
    TimestampFrame* timestampFrame) {
    TimestampFrame* lifetimeFrame = timestampFrame
        ? timestampFrame
        : g_activeResourceTimestampFrame;
    if (!realtime || MirrorUsesEveryFrameUpdates(mirror.fps) ||
        !lifetimeFrame) {
        return realtime;
    }
    const auto stateIt = g_state.mirrorSnapshots.find(mirror.fps);
    if (stateIt == g_state.mirrorSnapshots.end() ||
        stateIt->second.latestSlot == std::numeric_limits<size_t>::max()) {
        return realtime;
    }
    MirrorSnapshotSlot& slot =
        stateIt->second.slots[stateIt->second.latestSlot];
    if (!slot.image.initialized) return realtime;
    slot.pendingFrameMask |= TimestampFrameBit(lifetimeFrame);
    return &slot.image.sampled;
}

void DestroyTextureAsset(TextureAsset& asset) {
    ImGui::SetCurrentContext(g_state.imguiContext);
    for (TextureFrame& frame : asset.frames) {
        if (frame.sampled.descriptor) {
            ImGui_ImplVulkan_RemoveTexture(frame.sampled.descriptor);
        }
        if (frame.sampled.linearDescriptor) {
            ImGui_ImplVulkan_RemoveTexture(frame.sampled.linearDescriptor);
        }
        if (frame.sampled.view && g_state.dispatch.destroyImageView) {
            g_state.dispatch.destroyImageView(
                g_state.device, frame.sampled.view, nullptr);
        }
        if (frame.sampled.image && g_state.dispatch.destroyImage) {
            g_state.dispatch.destroyImage(
                g_state.device, frame.sampled.image, nullptr);
        }
        if (frame.memory && g_state.dispatch.freeMemory) {
            g_state.dispatch.freeMemory(g_state.device, frame.memory, nullptr);
        }
    }
    if (asset.stagingMapped && g_state.dispatch.unmapMemory) {
        g_state.dispatch.unmapMemory(g_state.device, asset.stagingMemory);
    }
    if (asset.stagingBuffer && g_state.dispatch.destroyBuffer) {
        g_state.dispatch.destroyBuffer(
            g_state.device, asset.stagingBuffer, nullptr);
    }
    if (asset.stagingMemory && g_state.dispatch.freeMemory) {
        g_state.dispatch.freeMemory(g_state.device, asset.stagingMemory, nullptr);
    }
    asset = {};
}

void RetireTextureAsset(TextureAsset&& asset) {
    const uint32_t pendingFrameMask = PendingTimestampFrameMask();
    if (pendingFrameMask == 0) {
        DestroyTextureAsset(asset);
        return;
    }
    RetiredTextureAsset retired{};
    retired.asset = std::move(asset);
    retired.pendingFrameMask = pendingFrameMask;
    g_state.retiredTextureAssets.push_back(std::move(retired));
}

RetiredFontResource TakeActiveFontResource(uint32_t pendingFrameMask) {
    RetiredFontResource resource{};
    resource.image = g_state.fontImage;
    resource.memory = g_state.fontMemory;
    resource.view = g_state.fontView;
    resource.descriptor = g_state.fontDescriptor;
    resource.uploadBuffer = g_state.fontUploadBuffer;
    resource.uploadMemory = g_state.fontUploadMemory;
    resource.uploadMapped = g_state.fontUploadMapped;
    resource.width = g_state.fontWidth;
    resource.height = g_state.fontHeight;
    resource.uploadRecorded = g_state.fontUploadRecorded;
    resource.pendingFrameMask = pendingFrameMask;
    g_state.fontImage = VK_NULL_HANDLE;
    g_state.fontMemory = VK_NULL_HANDLE;
    g_state.fontView = VK_NULL_HANDLE;
    g_state.fontDescriptor = VK_NULL_HANDLE;
    g_state.fontUploadBuffer = VK_NULL_HANDLE;
    g_state.fontUploadMemory = VK_NULL_HANDLE;
    g_state.fontUploadMapped = nullptr;
    g_state.fontWidth = 0;
    g_state.fontHeight = 0;
    g_state.fontUploadRecorded = false;
    return resource;
}

void RestoreActiveFontResource(RetiredFontResource&& resource) {
    g_state.fontImage = resource.image;
    g_state.fontMemory = resource.memory;
    g_state.fontView = resource.view;
    g_state.fontDescriptor = resource.descriptor;
    g_state.fontUploadBuffer = resource.uploadBuffer;
    g_state.fontUploadMemory = resource.uploadMemory;
    g_state.fontUploadMapped = resource.uploadMapped;
    g_state.fontWidth = resource.width;
    g_state.fontHeight = resource.height;
    g_state.fontUploadRecorded = resource.uploadRecorded;
    resource = {};
}

void DestroyFontResource(RetiredFontResource& resource) {
    ImGui::SetCurrentContext(g_state.imguiContext);
    if (resource.descriptor) {
        ImGui_ImplVulkan_RemoveTexture(resource.descriptor);
    }
    if (resource.uploadMapped && g_state.dispatch.unmapMemory) {
        g_state.dispatch.unmapMemory(
            g_state.device, resource.uploadMemory);
    }
    if (resource.uploadBuffer && g_state.dispatch.destroyBuffer) {
        g_state.dispatch.destroyBuffer(
            g_state.device, resource.uploadBuffer, nullptr);
    }
    if (resource.uploadMemory && g_state.dispatch.freeMemory) {
        g_state.dispatch.freeMemory(
            g_state.device, resource.uploadMemory, nullptr);
    }
    if (resource.view && g_state.dispatch.destroyImageView) {
        g_state.dispatch.destroyImageView(
            g_state.device, resource.view, nullptr);
    }
    if (resource.image && g_state.dispatch.destroyImage) {
        g_state.dispatch.destroyImage(
            g_state.device, resource.image, nullptr);
    }
    if (resource.memory && g_state.dispatch.freeMemory) {
        g_state.dispatch.freeMemory(
            g_state.device, resource.memory, nullptr);
    }
    resource = {};
}

void RefreshFontResourcesIfNeeded() {
    ImGui::SetCurrentContext(g_state.imguiContext);
    ApplyDynamicGuiFontRefresh();
    ApplyPendingKeyboardLayoutFontRefresh();
    ImGuiIO& io = ImGui::GetIO();
    const ImTextureID activeTexture = static_cast<ImTextureID>(
        reinterpret_cast<uintptr_t>(g_state.fontDescriptor));
    if (io.Fonts->TexData &&
        io.Fonts->TexData->GetTexID() == activeTexture) {
        return;
    }

    RetiredFontResource previous =
        TakeActiveFontResource(PendingTimestampFrameMask());
    if (CreateFontResources()) {
        if (previous.pendingFrameMask == 0) {
            DestroyFontResource(previous);
        } else {
            g_state.retiredFontResources.push_back(std::move(previous));
        }
        return;
    }

    RetiredFontResource failed = TakeActiveFontResource(0);
    DestroyFontResource(failed);
    RestoreActiveFontResource(std::move(previous));
    if (io.Fonts->TexData && g_state.fontDescriptor) {
        io.Fonts->TexData->SetTexID(activeTexture);
        io.Fonts->TexData->SetStatus(ImTextureStatus_OK);
    }
    LogCategory(
        "init",
        "[VULKAN] Dynamic font atlas rebuild failed; retaining the previous Vulkan font texture.");
}

bool CreateTextureFrame(int width, int height, TextureFrame& frame) {
    VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {
        static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!g_state.dispatch.createImage ||
        g_state.dispatch.createImage(
            g_state.device, &imageInfo, nullptr,
            &frame.sampled.image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements{};
    g_state.dispatch.getImageMemoryRequirements(
        g_state.device, frame.sampled.image, &requirements);
    uint32_t memoryType = 0;
    if (!FindMemoryType(
            requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            memoryType)) {
        return false;
    }
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (g_state.dispatch.allocateMemory(
            g_state.device, &allocation, nullptr, &frame.memory) != VK_SUCCESS ||
        g_state.dispatch.bindImageMemory(
            g_state.device, frame.sampled.image, frame.memory, 0) != VK_SUCCESS) {
        return false;
    }
    ImageMetadata metadata{};
    metadata.device = g_state.device;
    metadata.format = imageInfo.format;
    metadata.extent = imageInfo.extent;
    metadata.usage = imageInfo.usage;
    if (!CreateImageView(frame.sampled.image, metadata, frame.sampled)) {
        return false;
    }
    frame.sampled.descriptor = ImGui_ImplVulkan_AddTexture(
        g_state.mirrorSampler, frame.sampled.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    frame.sampled.linearDescriptor = ImGui_ImplVulkan_AddTexture(
        g_state.linearSampler, frame.sampled.view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    frame.sampled.descriptorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return frame.sampled.descriptor != VK_NULL_HANDLE &&
           frame.sampled.linearDescriptor != VK_NULL_HANDLE;
}

bool CreateTextureAsset(const DecodedImageData& decoded, TextureAsset& asset) {
    const int frameCount =
        decoded.isAnimated && decoded.frameCount > 1 ? decoded.frameCount : 1;
    const int frameHeight =
        decoded.frameHeight > 0 ? decoded.frameHeight : decoded.height;
    if (!decoded.data || decoded.width <= 0 || frameHeight <= 0 ||
        frameCount <= 0 || decoded.channels != 4) {
        return false;
    }
    const uint64_t frameBytes =
        static_cast<uint64_t>(decoded.width) *
        static_cast<uint64_t>(frameHeight) * 4ull;
    const uint64_t totalBytes = frameBytes * static_cast<uint64_t>(frameCount);
    if (totalBytes == 0 ||
        totalBytes > static_cast<uint64_t>((std::numeric_limits<size_t>::max)())) {
        return false;
    }

    asset.type = decoded.type;
    asset.width = decoded.width;
    asset.height = frameHeight;
    asset.isFullyTransparent = true;
    for (uint64_t pixel = 0; pixel < totalBytes / 4ull; ++pixel) {
        if (decoded.data[pixel * 4ull + 3ull] > 0) {
            asset.isFullyTransparent = false;
            break;
        }
    }
    asset.frameDelays = decoded.frameDelays;
    asset.frames.resize(static_cast<size_t>(frameCount));
    for (TextureFrame& frame : asset.frames) {
        if (!CreateTextureFrame(decoded.width, frameHeight, frame)) {
            DestroyTextureAsset(asset);
            return false;
        }
    }

    VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = static_cast<VkDeviceSize>(totalBytes);
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!g_state.dispatch.createBuffer ||
        g_state.dispatch.createBuffer(
            g_state.device, &bufferInfo, nullptr,
            &asset.stagingBuffer) != VK_SUCCESS) {
        DestroyTextureAsset(asset);
        return false;
    }
    VkMemoryRequirements requirements{};
    g_state.dispatch.getBufferMemoryRequirements(
        g_state.device, asset.stagingBuffer, &requirements);
    uint32_t memoryType = 0;
    if (!FindMemoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            memoryType)) {
        DestroyTextureAsset(asset);
        return false;
    }
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (g_state.dispatch.allocateMemory(
            g_state.device, &allocation, nullptr,
            &asset.stagingMemory) != VK_SUCCESS ||
        g_state.dispatch.bindBufferMemory(
            g_state.device, asset.stagingBuffer, asset.stagingMemory, 0) != VK_SUCCESS ||
        g_state.dispatch.mapMemory(
            g_state.device, asset.stagingMemory, 0, bufferInfo.size, 0,
            &asset.stagingMapped) != VK_SUCCESS) {
        DestroyTextureAsset(asset);
        return false;
    }
    memcpy(asset.stagingMapped, decoded.data, static_cast<size_t>(totalBytes));
    asset.stagingSize = bufferInfo.size;

    asset.frameEndTimesMs.resize(asset.frames.size());
    uint64_t cumulative = 0;
    for (size_t i = 0; i < asset.frames.size(); ++i) {
        const int delay = i < asset.frameDelays.size()
                              ? asset.frameDelays[i]
                              : 100;
        cumulative += static_cast<uint64_t>((std::max)(delay, 1));
        asset.frameEndTimesMs[i] = cumulative;
    }
    asset.totalDurationMs = cumulative;
    asset.animationStart = std::chrono::steady_clock::now();
    return true;
}

void ProcessPendingTextureAssets() {
    std::vector<DecodedImageData> pending;
    {
        std::lock_guard<std::mutex> lock(g_decodedImagesMutex);
        pending.swap(g_decodedImagesQueue);
    }
    for (DecodedImageData& decoded : pending) {
        if (decoded.data) {
            TextureAsset replacement;
            if (CreateTextureAsset(decoded, replacement)) {
                auto existing = g_state.textureAssets.find(decoded.id);
                if (existing != g_state.textureAssets.end()) {
                    RetireTextureAsset(std::move(existing->second));
                    existing->second = std::move(replacement);
                } else {
                    g_state.textureAssets.emplace(
                        decoded.id, std::move(replacement));
                }
            } else {
                LogCategory(
                    "image_monitor",
                    "[VULKAN] Failed to create texture asset '" + decoded.id + "'.");
            }
            stbi_image_free(decoded.data);
            decoded.data = nullptr;
        }
    }
}

void QueueBundledTextureAsset(
    const char* id, int resourceId) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&QueueBundledTextureAsset), &module) ||
        !module) {
        return;
    }
    const HRSRC resource = FindResourceW(
        module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) return;
    const HGLOBAL loaded = LoadResource(module, resource);
    const DWORD byteCount = SizeofResource(module, resource);
    const auto* bytes = static_cast<const unsigned char*>(
        loaded ? LockResource(loaded) : nullptr);
    if (!bytes || byteCount == 0 ||
        byteCount > static_cast<DWORD>((std::numeric_limits<int>::max)())) {
        return;
    }

    stbi_set_flip_vertically_on_load_thread(0);
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(
        bytes, static_cast<int>(byteCount), &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return;
    }

    DecodedImageData decoded{};
    decoded.type = DecodedImageData::UserImage;
    decoded.id = id;
    decoded.width = width;
    decoded.height = height;
    decoded.channels = 4;
    decoded.data = pixels;
    decoded.frameCount = 1;
    decoded.frameHeight = height;
    {
        std::lock_guard<std::mutex> lock(g_decodedImagesMutex);
        g_decodedImagesQueue.push_back(std::move(decoded));
    }
}

void RecordTextureUploads(VkCommandBuffer commandBuffer) {
    for (auto& [id, asset] : g_state.textureAssets) {
        if (asset.uploadRecorded || !asset.stagingBuffer) continue;
        const VkDeviceSize frameBytes =
            static_cast<VkDeviceSize>(asset.width) *
            static_cast<VkDeviceSize>(asset.height) * 4;
        for (size_t i = 0; i < asset.frames.size(); ++i) {
            TextureFrame& frame = asset.frames[i];
            VkImageMemoryBarrier toTransfer = MakeBarrier(
                frame.sampled.image, VK_ACCESS_NONE, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            g_state.dispatch.cmdPipelineBarrier(
                commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                &toTransfer);
            VkBufferImageCopy copy{};
            copy.bufferOffset = frameBytes * static_cast<VkDeviceSize>(i);
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {
                static_cast<uint32_t>(asset.width),
                static_cast<uint32_t>(asset.height), 1 };
            g_state.dispatch.cmdCopyBufferToImage(
                commandBuffer, asset.stagingBuffer, frame.sampled.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            VkImageMemoryBarrier toShader = MakeBarrier(
                frame.sampled.image, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            g_state.dispatch.cmdPipelineBarrier(
                commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                1, &toShader);
        }
        asset.uploadRecorded = true;
    }
}

TextureFrame* ResolveTextureFrame(TextureAsset& asset, bool linear) {
    if (!asset.uploadRecorded || asset.frames.empty()) return nullptr;
    if (g_obsCompositionPass) {
        const auto resolved =
            g_state.frameResolvedTextureFrames.find(&asset);
        if (resolved != g_state.frameResolvedTextureFrames.end()) {
            return resolved->second;
        }
    }
    size_t index = 0;
    if (asset.frames.size() > 1 && asset.totalDurationMs > 0) {
        const uint64_t elapsed = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - asset.animationStart)
                .count());
        const uint64_t position = elapsed % asset.totalDurationMs;
        index = static_cast<size_t>(
            std::lower_bound(
                asset.frameEndTimesMs.begin(), asset.frameEndTimesMs.end(),
                position + 1) -
            asset.frameEndTimesMs.begin());
        index = (std::min)(index, asset.frames.size() - 1);
    }
    TextureFrame& frame = asset.frames[index];
    if (linear && !frame.sampled.linearDescriptor) {
        frame.sampled.linearDescriptor = ImGui_ImplVulkan_AddTexture(
            g_state.linearSampler, frame.sampled.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if (!g_obsCompositionPass) {
        g_state.frameResolvedTextureFrames[&asset] = &frame;
    }
    return &frame;
}

void DestroyStreamingTextureSlot(StreamingTextureSlot& slot) {
    TextureAsset temporary;
    temporary.frames.push_back(std::move(slot.frame));
    temporary.stagingBuffer = slot.stagingBuffer;
    temporary.stagingMemory = slot.stagingMemory;
    temporary.stagingMapped = slot.stagingMapped;
    DestroyTextureAsset(temporary);
    slot = {};
}

bool CreateStreamingTextureSlot(
    int width, int height, StreamingTextureSlot& slot) {
    if (!CreateTextureFrame(width, height, slot.frame)) {
        DestroyStreamingTextureSlot(slot);
        return false;
    }
    const VkDeviceSize byteCount =
        static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;
    VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = byteCount;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_state.dispatch.createBuffer(
            g_state.device, &bufferInfo, nullptr,
            &slot.stagingBuffer) != VK_SUCCESS) {
        DestroyStreamingTextureSlot(slot);
        return false;
    }
    VkMemoryRequirements requirements{};
    g_state.dispatch.getBufferMemoryRequirements(
        g_state.device, slot.stagingBuffer, &requirements);
    uint32_t memoryType = 0;
    if (!FindMemoryType(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            memoryType)) {
        DestroyStreamingTextureSlot(slot);
        return false;
    }
    VkMemoryAllocateInfo allocation{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memoryType;
    if (g_state.dispatch.allocateMemory(
            g_state.device, &allocation, nullptr,
            &slot.stagingMemory) != VK_SUCCESS ||
        g_state.dispatch.bindBufferMemory(
            g_state.device, slot.stagingBuffer, slot.stagingMemory, 0) != VK_SUCCESS ||
        g_state.dispatch.mapMemory(
            g_state.device, slot.stagingMemory, 0, byteCount, 0,
            &slot.stagingMapped) != VK_SUCCESS) {
        DestroyStreamingTextureSlot(slot);
        return false;
    }
    slot.stagingSize = byteCount;
    slot.width = width;
    slot.height = height;
    return true;
}

TextureFrame* PrepareStreamingTexture(
    const std::string& key, VkImage frameSlot, const unsigned char* pixels,
    int width, int height, uint64_t generation, VkCommandBuffer commandBuffer,
    bool linear) {
    if (!frameSlot || !pixels || width <= 0 || height <= 0 || generation == 0) {
        return nullptr;
    }
    StreamingTexture& texture = g_state.streamingTextures[key];
    StreamingTextureSlot& slot = texture.slots[frameSlot];
    if (!slot.frame.sampled.image || slot.width != width || slot.height != height) {
        if (slot.frame.sampled.image) DestroyStreamingTextureSlot(slot);
        if (!CreateStreamingTextureSlot(width, height, slot)) return nullptr;
    }

    if (slot.uploadedGeneration != generation) {
        const size_t byteCount =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        memcpy(slot.stagingMapped, pixels, byteCount);
        VkImageMemoryBarrier toTransfer = MakeBarrier(
            slot.frame.sampled.image,
            slot.uploadedGeneration ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_NONE,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            slot.uploadedGeneration ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                    : VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        g_state.dispatch.cmdPipelineBarrier(
            commandBuffer,
            slot.uploadedGeneration ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                    : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &toTransfer);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {
            static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };
        g_state.dispatch.cmdCopyBufferToImage(
            commandBuffer, slot.stagingBuffer, slot.frame.sampled.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImageMemoryBarrier toShader = MakeBarrier(
            slot.frame.sampled.image, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        g_state.dispatch.cmdPipelineBarrier(
            commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &toShader);
        slot.uploadedGeneration = generation;
    }
    if (linear && !slot.frame.sampled.linearDescriptor) {
        slot.frame.sampled.linearDescriptor = ImGui_ImplVulkan_AddTexture(
            g_state.linearSampler, slot.frame.sampled.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    return &slot.frame;
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
    if (!context.swapchain || !context.dispatch || !context.destinationMetadata || snapshot->instance == VK_NULL_HANDLE) {
        LogInitializeFailure("incomplete final-blit or instance tracking");
        return false;
    }

    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = VK_QUEUE_FAMILY_IGNORED;
    for (const auto& [candidate, metadata] : snapshot->queues) {
        if (metadata.device == context.device &&
            (context.commandBufferQueueFamily == VK_QUEUE_FAMILY_IGNORED ||
             metadata.familyIndex == context.commandBufferQueueFamily)) {
            queue = candidate;
            family = metadata.familyIndex;
            break;
        }
    }
    if (!queue || family == VK_QUEUE_FAMILY_IGNORED) {
        LogInitializeFailure("no tracked graphics submission queue");
        return false;
    }

    HWND hwnd = context.swapchain->hwnd;
    if (!IsTrackedMinecraftWindow(hwnd)) {
        LogInitializeFailure("the final-blit swapchain has no valid tracked Win32 surface");
        return false;
    }

    g_state.instance = snapshot->instance;
    g_state.device = context.device;
    g_state.physicalDevice = context.dispatch->physicalDevice;
    g_state.queue = queue;
    g_state.queueFamily = family;
    g_state.format = context.swapchain->format;
    g_state.imageCount = static_cast<uint32_t>(context.swapchain->images.size());
    g_state.activeTimestampFrames = std::clamp(
        g_state.imageCount > 0 ? g_state.imageCount
                               : context.swapchain->minImageCount,
        1u, kMaxTimestampFrames);
    g_state.dispatch = *context.dispatch;
    g_state.hwnd = hwnd;

    const auto failInitialization = [&](const char* reason) {
        LogInitializeFailure(reason);
        VulkanRenderer::Shutdown();
        return false;
    };

    if (g_state.imageCount < 2) g_state.imageCount = (std::max)(2u, context.swapchain->minImageCount);

    const bool hasDynamicRendering =
        (g_state.dispatch.cmdBeginRendering && g_state.dispatch.cmdEndRendering) ||
        (g_state.dispatch.cmdBeginRenderingKHR && g_state.dispatch.cmdEndRenderingKHR);
    if (!g_state.dispatch.createSampler || !g_state.dispatch.destroySampler ||
        !g_state.dispatch.createImage || !g_state.dispatch.destroyImage ||
        !g_state.dispatch.createImageView || !g_state.dispatch.destroyImageView ||
        !g_state.dispatch.createBuffer || !g_state.dispatch.destroyBuffer ||
        !g_state.dispatch.getImageMemoryRequirements ||
        !g_state.dispatch.getBufferMemoryRequirements ||
        !g_state.dispatch.allocateMemory || !g_state.dispatch.freeMemory ||
        !g_state.dispatch.bindImageMemory || !g_state.dispatch.bindBufferMemory ||
        !g_state.dispatch.mapMemory || !g_state.dispatch.unmapMemory ||
        !g_state.dispatch.cmdPipelineBarrier ||
        !g_state.dispatch.cmdCopyBufferToImage || !g_state.dispatch.createQueryPool ||
        !g_state.dispatch.destroyQueryPool || !g_state.dispatch.getQueryPoolResults ||
        !g_state.dispatch.cmdResetQueryPool || !g_state.dispatch.cmdWriteTimestamp ||
        !hasDynamicRendering) {
        return failInitialization("required Vulkan resource or dynamic-rendering dispatch is unavailable");
    }

    auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        VulkanHooks::LoadRealFunction(
            "vkGetPhysicalDeviceProperties", reinterpret_cast<void*>(g_state.device)));
    if (!getProperties) return failInitialization("physical-device property dispatch is unavailable");
    VkPhysicalDeviceProperties properties{};
    getProperties(g_state.physicalDevice, &properties);
    g_state.timestampPeriodNs = properties.limits.timestampPeriod;
    if (g_state.timestampPeriodNs <= 0.0f) {
        return failInitialization("timestamp queries are unsupported by the physical device");
    }
    auto getQueueProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
        VulkanHooks::LoadRealFunction(
            "vkGetPhysicalDeviceQueueFamilyProperties", reinterpret_cast<void*>(g_state.device)));
    uint32_t queueFamilyCount = 0;
    if (!getQueueProperties) return failInitialization("queue-family property dispatch is unavailable");
    getQueueProperties(g_state.physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueProperties(queueFamilyCount);
    if (g_state.queueFamily >= queueFamilyCount) {
        return failInitialization("tracked command-buffer queue family is unavailable");
    }
    getQueueProperties(g_state.physicalDevice, &queueFamilyCount, queueProperties.data());
    if (queueProperties[g_state.queueFamily].timestampValidBits == 0) {
        return failInitialization("tracked command-buffer queue family has no timestamp support");
    }
    const bool coreDynamicRendering =
        VK_VERSION_MAJOR(properties.apiVersion) > 1 ||
        (VK_VERSION_MAJOR(properties.apiVersion) == 1 &&
         VK_VERSION_MINOR(properties.apiVersion) >= 3);
    const uint32_t imguiApiVersion =
        coreDynamicRendering ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;

    if (!ImGui_ImplVulkan_LoadFunctions(
            imguiApiVersion, ImGuiLoader, reinterpret_cast<void*>(g_state.device))) {
        return failInitialization("ImGui could not load the Vulkan device dispatch");
    }

    IMGUI_CHECKVERSION();
    g_state.imguiContext = ImGui::CreateContext();
    if (!g_state.imguiContext) return failInitialization("ImGui context creation failed");
    ImGui::SetCurrentContext(g_state.imguiContext);
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ConfigureImGuiFontsAndStyleForCurrentContext(ComputeGuiScaleFactorFromCachedWindowSize());
    if (!ImGui_ImplWin32_Init(hwnd)) {
        return failInitialization("ImGui Win32 backend initialization failed");
    }
    g_state.imguiWin32Initialized = true;

    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = imguiApiVersion;
    init.Instance = g_state.instance;
    init.PhysicalDevice = g_state.physicalDevice;
    init.Device = g_state.device;
    init.QueueFamily = g_state.queueFamily;
    init.Queue = g_state.queue;
    init.DescriptorPoolSize = 2048;
    init.MinImageCount = (std::max)(2u, context.swapchain->minImageCount);
    init.ImageCount = g_state.imageCount;
    init.UseDynamicRendering = true;
    init.CheckVkResultFn = CheckVkResult;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_state.format;
    if (!ImGui_ImplVulkan_Init(&init)) {
        return failInitialization("ImGui Vulkan dynamic-rendering initialization failed");
    }
    g_state.imguiVulkanInitialized = true;

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
        return failInitialization("nearest-sampler creation failed");
    }
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (g_state.dispatch.createSampler(
            g_state.device, &sampler, nullptr,
            &g_state.linearSampler) != VK_SUCCESS) {
        return failInitialization("linear-sampler creation failed");
    }
    if (!CreateFontResources()) {
        Log("[VULKAN] Failed to create the persistent ImGui font upload resources.");
        return failInitialization("persistent ImGui font upload resource creation failed");
    }
    // This context shares the atlas and style but has no Win32/Vulkan backend.
    // It is used only for the pass-specific OBS composition primitives.
    g_state.obsImGuiContext = ImGui::CreateContext(ImGui::GetIO().Fonts);
    if (!g_state.obsImGuiContext) return failInitialization("passive OBS ImGui context creation failed");
    ImGui::SetCurrentContext(g_state.obsImGuiContext);
    ImGui::GetCurrentContext()->Style = g_state.imguiContext->Style;
    ImGui::GetIO().Fonts->TexData->SetTexID(
        static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(g_state.fontDescriptor)));
    ImGui::SetCurrentContext(g_state.imguiContext);
    Log("[VULKAN][GUI] Created persistent interactive and passive OBS ImGui contexts; only the interactive context owns Win32 input.");
    if (!CreateMirrorPipelineResources()) {
        Log("[VULKAN] Failed to create the filtered mirror pipeline resources.");
        return failInitialization("filtered mirror pipeline resource creation failed");
    }
    // Optional only on devices exposing VK_EXT_external_memory_host. Failure
    // leaves ordinary Vulkan and OpenGL rendering intact; it never enables the
    // prohibited CPU full-frame fallback.
    (void)CreateVirtualCameraPipelineResources();
    if (!CreateColorPickerReadbackResources()) {
        Log("[VULKAN] Color-picker readback resources are unavailable.");
    }
    VkQueryPoolCreateInfo queryInfo{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryInfo.queryCount = g_state.activeTimestampFrames * kQueriesPerFrame;
    if (g_state.dispatch.createQueryPool(
            g_state.device, &queryInfo, nullptr, &g_state.queryPool) != VK_SUCCESS ||
        !g_state.queryPool) {
        return failInitialization("timestamp query-pool creation failed");
    }

    queryInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
    queryInfo.queryCount = g_state.activeTimestampFrames * kMaxMirrorQueriesPerFrame;
    if (g_state.dispatch.createQueryPool(
            g_state.device, &queryInfo, nullptr, &g_state.mirrorQueryPool) != VK_SUCCESS) {
        g_state.mirrorQueryPool = VK_NULL_HANDLE;
        Log("[VULKAN] Occlusion-query pool unavailable; mirror content tests will be disabled.");
    }
    g_state.cmdBeginQuery = reinterpret_cast<PFN_vkCmdBeginQuery>(
        VulkanHooks::LoadRealFunction("vkCmdBeginQuery", reinterpret_cast<void*>(g_state.device)));
    g_state.cmdEndQuery = reinterpret_cast<PFN_vkCmdEndQuery>(
        VulkanHooks::LoadRealFunction("vkCmdEndQuery", reinterpret_cast<void*>(g_state.device)));
    g_state.cmdCopyImage = reinterpret_cast<PFN_vkCmdCopyImage>(
        VulkanHooks::LoadRealFunction("vkCmdCopyImage", reinterpret_cast<void*>(g_state.device)));
    g_minecraftHwnd.store(hwnd, std::memory_order_release);
    SubclassGameWindow(hwnd);
    g_allImagesLoaded.store(false, std::memory_order_release);
    g_pendingImageLoad.store(true, std::memory_order_release);
    g_state.initialized = true;
    g_ready.store(true, std::memory_order_release);
    g_loggedInitializeFailure.store(false, std::memory_order_release);
    Log("[VULKAN] Native dynamic-rendering backend initialized.");
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
        if (frame.screenshotPending && g_state.screenshotReadbackMapped) {
            const bool bgra =
                frame.screenshotFormat == VK_FORMAT_B8G8R8A8_UNORM ||
                frame.screenshotFormat == VK_FORMAT_B8G8R8A8_SRGB;
            ScreenshotPixelsToClipboard(
                g_state.screenshotReadbackMapped, frame.screenshotWidth,
                frame.screenshotHeight,
                static_cast<size_t>(frame.screenshotWidth) * 4u, bgra, true);
        }
        if (frame.virtualCameraPending) {
            const bool published =
                PublishVirtualCameraGpuFrame(frame.virtualCameraFrame);
            if (published) {
                ++g_state.virtualCameraPublishedCount;
            } else {
                ++g_state.virtualCameraDroppedCount;
            }
            {
                std::lock_guard lock(g_obsCompositionMutex);
                if (frame.virtualCameraCompositionSlot <
                    kObsCompositionSlotCount) {
                    ObsCompositionSlot& composition =
                        g_state.obsCompositionSlots[
                            frame.virtualCameraCompositionSlot];
                    if (composition.serial ==
                            frame.virtualCameraCompositionSerial &&
                        composition.virtualCameraReaderCount != 0) {
                        --composition.virtualCameraReaderCount;
                    }
                }
            }
            if (ShouldLogObsCapture(g_lastObsCompositionLogTick)) {
                Log("[VULKAN][VIRTUALCAM] GPU-complete NV12 frame " +
                    std::string(published ? "published" : "discarded") +
                    " directly from imported shared memory: recorded=" +
                    std::to_string(g_state.virtualCameraRecordedCount) +
                    ", published=" +
                    std::to_string(g_state.virtualCameraPublishedCount) +
                    ", dropped=" +
                    std::to_string(g_state.virtualCameraDroppedCount) + ".");
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
                    bool firstForMirror = true;
                    for (uint32_t previous = 0; previous < i; ++previous) {
                        if (frame.mirrorKeys[previous] == frame.mirrorKeys[i]) {
                            firstForMirror = false;
                            break;
                        }
                    }
                    if (firstForMirror) {
                        g_state.mirrorHasContent[frame.mirrorKeys[i]] = samples[i] != 0;
                    } else if (samples[i] != 0) {
                        g_state.mirrorHasContent[frame.mirrorKeys[i]] = true;
                    }
                }
            }
        }
        if (frame.compositionWritePending) {
            std::lock_guard lock(g_obsCompositionMutex);
            if (frame.compositionWriteSlot < kObsCompositionSlotCount) {
                ObsCompositionSlot& composition =
                    g_state.obsCompositionSlots[frame.compositionWriteSlot];
                if (composition.serial == frame.compositionWriteSerial) {
                    composition.gameWriterPending = false;
                }
            }
        }
        const double periodMs = static_cast<double>(g_state.timestampPeriodNs) / 1'000'000.0;
        Profiler::GetInstance().SubmitExternalTiming(
            "Vulkan GPU final blit", static_cast<double>(values[2] - values[0]) * periodMs);
        Profiler::GetInstance().SubmitExternalTiming(
            "Vulkan GPU mirror preparation/copy",
            static_cast<double>(values[4] - values[2]) * periodMs);
        Profiler::GetInstance().SubmitExternalTiming(
            "Vulkan GPU general uploads",
            static_cast<double>(values[6] - values[4]) * periodMs);
        Profiler::GetInstance().SubmitExternalTiming(
            "Vulkan GPU overlay preparation/uploads",
            static_cast<double>(values[8] - values[6]) * periodMs);
        Profiler::GetInstance().SubmitExternalTiming(
            "Vulkan GPU overlay rendering",
            static_cast<double>(values[10] - values[8]) * periodMs);
        Profiler::GetInstance().SubmitExternalTiming(
            "Vulkan GPU overlay completion",
            static_cast<double>(values[10] - values[0]) * periodMs);
        const uint32_t frameIndex = frame.firstQuery / kQueriesPerFrame;
        const uint32_t completedFrameBit =
            frameIndex < kMaxTimestampFrames ? (1u << frameIndex) : 0;
        if (completedFrameBit != 0) {
            for (auto& [fps, snapshot] : g_state.mirrorSnapshots) {
                for (MirrorSnapshotSlot& slot : snapshot.slots) {
                    slot.pendingFrameMask &= ~completedFrameBit;
                }
            }
            for (RetiredMirrorSnapshot& retired :
                 g_state.retiredMirrorSnapshots) {
                retired.pendingFrameMask &= ~completedFrameBit;
            }
            for (RetiredTextureAsset& retired :
                 g_state.retiredTextureAssets) {
                retired.pendingFrameMask &= ~completedFrameBit;
            }
            for (RetiredFontResource& retired :
                 g_state.retiredFontResources) {
                retired.pendingFrameMask &= ~completedFrameBit;
            }
        }
        frame.pending = false;
        frame.pickerPending = false;
        frame.screenshotPending = false;
        frame.screenshotWidth = 0;
        frame.screenshotHeight = 0;
        frame.screenshotFormat = VK_FORMAT_UNDEFINED;
        frame.virtualCameraPending = false;
        frame.virtualCameraFrame = {};
        frame.virtualCameraCompositionSlot = UINT32_MAX;
        frame.virtualCameraCompositionSerial = 0;
        frame.compositionWritePending = false;
        frame.compositionWriteSlot = UINT32_MAX;
        frame.compositionWriteSerial = 0;
        frame.mirrorQueryCount = 0;
        frame.commandBuffer = VK_NULL_HANDLE;
    }
    for (auto it = g_state.retiredMirrorSnapshots.begin();
         it != g_state.retiredMirrorSnapshots.end();) {
        if (it->pendingFrameMask == 0) {
            DestroyMirrorCopyImage(it->image);
            it = g_state.retiredMirrorSnapshots.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_state.retiredTextureAssets.begin();
         it != g_state.retiredTextureAssets.end();) {
        if (it->pendingFrameMask == 0) {
            DestroyTextureAsset(it->asset);
            it = g_state.retiredTextureAssets.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_state.retiredFontResources.begin();
         it != g_state.retiredFontResources.end();) {
        if (it->pendingFrameMask == 0) {
            DestroyFontResource(*it);
            it = g_state.retiredFontResources.erase(it);
        } else {
            ++it;
        }
    }
}

bool HasPendingGpuWork() {
    return std::any_of(
        g_state.timestamps.begin(), g_state.timestamps.end(),
        [](const TimestampFrame& frame) { return frame.pending; });
}

TimestampFrame* BeginTimestamps(VkCommandBuffer commandBuffer) {
    HarvestTimestamps();
    if (!g_state.queryPool || !g_state.dispatch.cmdResetQueryPool || !g_state.dispatch.cmdWriteTimestamp) return nullptr;
    for (uint32_t attempt = 0; attempt < g_state.activeTimestampFrames; ++attempt) {
        uint32_t index =
            (g_state.nextTimestamp + attempt) % g_state.activeTimestampFrames;
        TimestampFrame& frame = g_state.timestamps[index];
        if (frame.pending) continue;
        frame.commandBuffer = commandBuffer;
        frame.firstQuery = index * kQueriesPerFrame;
        frame.mirrorFirstQuery = index * kMaxMirrorQueriesPerFrame;
        frame.mirrorQueryCount = 0;
        frame.pickerPending = false;
        frame.screenshotPending = false;
        frame.virtualCameraPending = false;
        frame.virtualCameraFrame = {};
        frame.virtualCameraCompositionSlot = UINT32_MAX;
        frame.virtualCameraCompositionSerial = 0;
        frame.compositionWritePending = false;
        frame.compositionWriteSlot = UINT32_MAX;
        frame.compositionWriteSerial = 0;
        frame.pending = true;
        g_state.nextTimestamp =
            (index + 1) % g_state.activeTimestampFrames;
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

ModeViewportInfo ResolveSubmittedViewport(
    const VulkanRenderer::FinalBlitContext& context,
    bool forceObsAnimation = false) {
    ModeViewportInfo viewport{};
    if (g_state.configSnapshot &&
        !g_state.pendingSourceModeId.empty() &&
        g_state.modeId != g_state.publishedModeId) {
        const ModeConfig* visualMode = GetModeFromSnapshotOrFallback(
            *g_state.configSnapshot, g_state.modeId);
        if (visualMode) {
            const int swapW = static_cast<int>(context.swapchain->extent.width);
            const int swapH = static_cast<int>(context.swapchain->extent.height);
            viewport.valid = true;
            viewport.x = 0;
            viewport.y = 0;
            viewport.width = visualMode->width;
            viewport.height = visualMode->height;
            viewport.stretchEnabled = visualMode->stretch.enabled;
            if (visualMode->stretch.enabled) {
                if (EqualsIgnoreCase(visualMode->id, "Fullscreen")) {
                    viewport.stretchX = 0;
                    viewport.stretchY = 0;
                    viewport.stretchWidth = swapW;
                    viewport.stretchHeight = swapH;
                } else {
                    viewport.stretchX = visualMode->stretch.x;
                    viewport.stretchY = visualMode->stretch.y;
                    viewport.stretchWidth = visualMode->stretch.width;
                    viewport.stretchHeight = visualMode->stretch.height;
                }
            } else {
                viewport.stretchX =
                    GetCenteredAxisOffset(swapW, visualMode->width);
                viewport.stretchY =
                    GetCenteredAxisOffset(swapH, visualMode->height);
                viewport.stretchWidth = visualMode->width;
                viewport.stretchHeight = visualMode->height;
            }
        }
    }
    if (!viewport.valid && !ResolvePresentedGameViewport(viewport)) {
        viewport = GetCurrentModeViewport();
    }

    const ModeTransitionState transition = GetModeTransitionState();
    const bool useObsAnimatedViewport =
        (g_obsCompositionPass || forceObsAnimation) &&
        transition.active &&
        transition.gameTransition == GameTransitionType::Bounce;
    if (useObsAnimatedViewport) {
        viewport.valid =
            transition.width > 0 && transition.height > 0;
        viewport.x = transition.x;
        viewport.y = transition.y;
        viewport.width = transition.width;
        viewport.height = transition.height;
        viewport.stretchEnabled = true;
        viewport.stretchX = transition.x;
        viewport.stretchY = transition.y;
        viewport.stretchWidth = transition.width;
        viewport.stretchHeight = transition.height;
    }

    const int swapW = static_cast<int>(context.swapchain->extent.width);
    const int swapH = static_cast<int>(context.swapchain->extent.height);
    const int x = viewport.valid ? viewport.stretchX : 0;
    const int y = viewport.valid ? viewport.stretchY : 0;
    const int width = viewport.valid ? viewport.stretchWidth : swapW;
    const int height = viewport.valid ? viewport.stretchHeight : swapH;
    // Keep the logical viewport intact here. Modes can intentionally extend
    // beyond the physical swapchain (for example a tall Thin viewport centered
    // in a short window). Overlay anchors must continue to use that complete
    // rectangle; BuildAdjustedBlit clips the physical copy and crops its source
    // proportionally.
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

ModeViewportInfo ResolveOverlayViewport(
    const VulkanRenderer::FinalBlitContext& context) {
    ModeViewportInfo viewport = ResolveSubmittedViewport(context);
    const ModeTransitionState transition = GetModeTransitionState();
    if (!transition.active ||
        transition.overlayTransition != OverlayTransitionType::Cut) {
        return viewport;
    }

    const int x = transition.targetX;
    const int y = transition.targetY;
    const int width = transition.targetWidth;
    const int height = transition.targetHeight;
    if (width <= 0 || height <= 0) return viewport;
    viewport.valid = true;
    viewport.x = x;
    viewport.y = y;
    viewport.width = width;
    viewport.height = height;
    viewport.stretchEnabled = true;
    viewport.stretchX = x;
    viewport.stretchY = y;
    viewport.stretchWidth = width;
    viewport.stretchHeight = height;
    return viewport;
}

VkImageBlit BuildAdjustedBlit(
    const VulkanRenderer::FinalBlitContext& context,
    bool forceObsAnimation = false) {
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
    PROFILE_SCOPE_CAT("Vulkan viewport calculation", "Vulkan");
    const ModeViewportInfo viewport =
        ResolveSubmittedViewport(context, forceObsAnimation);
    const int logicalX = viewport.x;
    const int logicalY = viewport.y;
    const int logicalW = (std::max)(1, viewport.width);
    const int logicalH = (std::max)(1, viewport.height);
    const int swapW = static_cast<int>(context.swapchain->extent.width);
    const int swapH = static_cast<int>(context.swapchain->extent.height);
    const int clippedLeft = std::clamp(logicalX, 0, swapW);
    const int clippedTop = std::clamp(logicalY, 0, swapH);
    const int clippedRight =
        std::clamp(logicalX + logicalW, clippedLeft, swapW);
    const int clippedBottom =
        std::clamp(logicalY + logicalH, clippedTop, swapH);

    const auto mapSourceX = [&](int destinationX) {
        const double ratio =
            static_cast<double>(destinationX - logicalX) /
            static_cast<double>(logicalW);
        return std::clamp(
            static_cast<int>(std::lround(ratio * sourceW)), 0, sourceW);
    };
    const auto mapSourceY = [&](int destinationY) {
        // Minecraft's source is vertically flipped by the presentation blit.
        const double ratio =
            static_cast<double>(logicalY + logicalH - destinationY) /
            static_cast<double>(logicalH);
        return std::clamp(
            static_cast<int>(std::lround(ratio * sourceH)), 0, sourceH);
    };

    adjusted.srcOffsets[0] = {
        mapSourceX(clippedLeft), mapSourceY(clippedBottom), 0
    };
    adjusted.srcOffsets[1] = {
        mapSourceX(clippedRight), mapSourceY(clippedTop), 1
    };

    // Vulkan framebuffer coordinates are top-left. Minecraft's presentation
    // blit is vertically flipped, so retain the reversed destination Y pair.
    adjusted.dstOffsets[0] = { clippedLeft, clippedBottom, 0 };
    adjusted.dstOffsets[1] = { clippedRight, clippedTop, 1 };
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

void RefreshModeCache(int screenW, int screenH, int sourceW, int sourceH) {
    const uint64_t version = g_configSnapshotVersion.load(std::memory_order_acquire);
    const std::string publishedMode = GetPublishedCurrentModeId();
    const bool imagesVisible =
        g_imageOverlaysVisible.load(std::memory_order_acquire);
    const bool windowOverlaysVisible =
        g_windowOverlaysVisible.load(std::memory_order_acquire);
    const bool browserOverlaysVisible =
        g_browserOverlaysVisible.load(std::memory_order_acquire);
    if (g_state.configSnapshot &&
        g_state.pendingSourceModeId.empty() &&
        version == g_state.configVersion &&
        publishedMode == g_state.publishedModeId &&
        sourceW == g_state.modeCacheSourceWidth &&
        sourceH == g_state.modeCacheSourceHeight &&
        screenW == g_state.modeCacheScreenWidth &&
        screenH == g_state.modeCacheScreenHeight &&
        imagesVisible == g_state.modeCacheImagesVisible &&
        windowOverlaysVisible == g_state.modeCacheWindowOverlaysVisible &&
        browserOverlaysVisible == g_state.modeCacheBrowserOverlaysVisible) {
        return;
    }
    const bool configChanged =
        !g_state.configSnapshot || version != g_state.configVersion;
    const std::string previousVisualMode = g_state.modeId;
    const std::string previousPublishedMode = g_state.publishedModeId;
    g_state.configSnapshot = GetConfigSnapshot();
    g_state.configVersion = version;
    g_state.publishedModeId = publishedMode;
    const auto sourceMatchesMode = [&](const std::string& modeId) {
        if (!g_state.configSnapshot || modeId.empty() ||
            sourceW <= 0 || sourceH <= 0) {
            return false;
        }
        const ModeConfig* mode = GetModeFromSnapshotOrFallback(
            *g_state.configSnapshot, modeId);
        return mode && mode->width == sourceW && mode->height == sourceH;
    };

    const bool publishedModeChanged =
        !previousPublishedMode.empty() &&
        !EqualsIgnoreCase(previousPublishedMode, publishedMode);
    if (publishedModeChanged && !previousVisualMode.empty() &&
        sourceMatchesMode(previousVisualMode) &&
        !sourceMatchesMode(publishedMode)) {
        g_state.modeId = previousVisualMode;
        g_state.pendingSourceModeId = publishedMode;
        g_state.pendingSourceModeSince = std::chrono::steady_clock::now();
    } else if (!g_state.pendingSourceModeId.empty() &&
               EqualsIgnoreCase(
                   g_state.pendingSourceModeId, publishedMode)) {
        const bool targetSourceReady = sourceMatchesMode(publishedMode);
        const bool sourceNoLongerMatchesPrevious =
            !sourceMatchesMode(previousVisualMode);
        const bool timedOut =
            std::chrono::steady_clock::now() -
                g_state.pendingSourceModeSince >=
            std::chrono::milliseconds(500);
        if (targetSourceReady || sourceNoLongerMatchesPrevious || timedOut) {
            g_state.modeId = publishedMode;
            g_state.pendingSourceModeId.clear();
        }
    } else {
        g_state.modeId = publishedMode;
        g_state.pendingSourceModeId.clear();
    }
    const bool modeOrGeometryChanged =
        configChanged || previousVisualMode != g_state.modeId ||
        screenW != g_state.modeCacheScreenWidth ||
        screenH != g_state.modeCacheScreenHeight ||
        sourceW != g_state.modeCacheSourceWidth ||
        sourceH != g_state.modeCacheSourceHeight;
    g_state.modeCacheSourceWidth = sourceW;
    g_state.modeCacheSourceHeight = sourceH;
    g_state.modeCacheScreenWidth = screenW;
    g_state.modeCacheScreenHeight = screenH;
    g_state.modeCacheImagesVisible = imagesVisible;
    g_state.modeCacheWindowOverlaysVisible = windowOverlaysVisible;
    g_state.modeCacheBrowserOverlaysVisible = browserOverlaysVisible;
    g_state.mirrors.clear();
    g_state.slideOutMirrors.clear();
    g_state.slideOutFromModeId.clear();
    if (modeOrGeometryChanged) {
        g_state.mirrorHasContent.clear();
    }
    g_state.images.clear();
    g_state.windowOverlays.clear();
    g_state.browserOverlays.clear();
    if (g_state.configSnapshot) {
        CollectActiveElementsForMode(*g_state.configSnapshot, g_state.modeId, false, version, g_state.mirrors, g_state.images,
                                     g_state.windowOverlays, g_state.browserOverlays, screenW, screenH);
        const ModeTransitionState transition = GetModeTransitionState();
        if (transition.active &&
            transition.gameTransition == GameTransitionType::Bounce &&
            !transition.fromModeId.empty()) {
            const ModeConfig* fromMode = GetModeFromSnapshotOrFallback(
                *g_state.configSnapshot, transition.fromModeId);
            const bool wantsSlideOut =
                (fromMode && fromMode->slideMirrorsIn) ||
                (EqualsIgnoreCase(transition.fromModeId, "EyeZoom") &&
                 g_state.configSnapshot->eyezoom.slideMirrorsIn);
            if (wantsSlideOut) {
                std::vector<ImageConfig> unusedImages;
                std::vector<const WindowOverlayConfig*> unusedWindowOverlays;
                std::vector<const BrowserOverlayConfig*> unusedBrowserOverlays;
                CollectActiveElementsForMode(
                    *g_state.configSnapshot, transition.fromModeId, false,
                    version, g_state.slideOutMirrors, unusedImages,
                    unusedWindowOverlays, unusedBrowserOverlays, screenW,
                    screenH);
                std::unordered_set<std::string> targetMirrorNames;
                targetMirrorNames.reserve(g_state.mirrors.size());
                for (const MirrorConfig& target : g_state.mirrors) {
                    targetMirrorNames.insert(target.name);
                }
                std::erase_if(
                    g_state.slideOutMirrors,
                    [&](const MirrorConfig& outgoing) {
                        return targetMirrorNames.contains(outgoing.name);
                    });
                g_state.slideOutFromModeId = transition.fromModeId;
            }
        }
        if (configChanged) {
            std::unordered_set<std::string> allowedTextureIds;
            allowedTextureIds.reserve(
                g_state.configSnapshot->modes.size() +
                g_state.configSnapshot->images.size() +
                g_state.configSnapshot->eyezoom.overlays.size());
            for (const ModeConfig& configuredMode :
                 g_state.configSnapshot->modes) {
                if (configuredMode.background.selectedMode == "image") {
                    allowedTextureIds.insert(configuredMode.id);
                }
            }
            for (const ImageConfig& image : g_state.configSnapshot->images) {
                allowedTextureIds.insert(image.name);
            }
            for (const EyeZoomOverlayConfig& overlay :
                 g_state.configSnapshot->eyezoom.overlays) {
                allowedTextureIds.insert("ezoverlay_" + overlay.name);
            }
            const int indicatorMode =
                g_state.configSnapshot->keyRebinds.indicatorMode;
            if (indicatorMode == 1 || indicatorMode == 3) {
                allowedTextureIds.insert(kRebindIndicatorEnabledTextureId);
                QueueBundledTextureAsset(
                    kRebindIndicatorEnabledTextureId,
                    IDR_REBIND_ON_PNG);
                if (!g_state.configSnapshot->keyRebinds
                         .indicatorImageEnabled.empty()) {
                    LoadImageAsync(
                        DecodedImageData::UserImage,
                        kRebindIndicatorEnabledTextureId,
                        g_state.configSnapshot->keyRebinds
                            .indicatorImageEnabled,
                        g_toolscreenPath);
                }
            }
            if (indicatorMode == 2 || indicatorMode == 3) {
                allowedTextureIds.insert(kRebindIndicatorDisabledTextureId);
                QueueBundledTextureAsset(
                    kRebindIndicatorDisabledTextureId,
                    IDR_REBIND_OFF_PNG);
                if (!g_state.configSnapshot->keyRebinds
                         .indicatorImageDisabled.empty()) {
                    LoadImageAsync(
                        DecodedImageData::UserImage,
                        kRebindIndicatorDisabledTextureId,
                        g_state.configSnapshot->keyRebinds
                            .indicatorImageDisabled,
                        g_toolscreenPath);
                }
            }
            if (g_showGui.load(std::memory_order_acquire)) {
                allowedTextureIds.insert(kGuiLanguageTextureId);
                allowedTextureIds.insert(kGuiDiscordTextureId);
                allowedTextureIds.insert(kGuiEditorTextureId);
                QueueBundledTextureAsset(
                    kGuiLanguageTextureId, IDR_LANGUAGE_PNG);
                QueueBundledTextureAsset(
                    kGuiDiscordTextureId, IDR_DISCORD_PNG);
                QueueBundledTextureAsset(
                    kGuiEditorTextureId, IDR_EDITOR_PNG);
            }
            if (g_state.configSnapshot->cursorTrail.enabled &&
                !g_state.configSnapshot->cursorTrail.spritePath.empty()) {
                allowedTextureIds.insert(kCursorTrailTextureId);
                LoadImageAsync(
                    DecodedImageData::UserImage, kCursorTrailTextureId,
                    g_state.configSnapshot->cursorTrail.spritePath,
                    g_toolscreenPath);
            }
            if (g_state.configSnapshot->startupIndicatorMode == 2 &&
                !g_state.configSnapshot->startupIndicatorImagePath.empty()) {
                allowedTextureIds.insert(kStartupIndicatorTextureId);
                LoadImageAsync(
                    DecodedImageData::UserImage,
                    kStartupIndicatorTextureId,
                    g_state.configSnapshot->startupIndicatorImagePath,
                    g_toolscreenPath);
            }
            if (g_state.configSnapshot->ninjabrainOverlay.enabled) {
                for (size_t index = 0;
                     index < kNinjabrainBoatTextureIds.size(); ++index) {
                    allowedTextureIds.insert(kNinjabrainBoatTextureIds[index]);
                    QueueBundledTextureAsset(
                        kNinjabrainBoatTextureIds[index],
                        kNinjabrainBoatResourceIds[index]);
                }
                for (size_t index = 0;
                     index < kNinjabrainMessageTextureIds.size(); ++index) {
                    allowedTextureIds.insert(
                        kNinjabrainMessageTextureIds[index]);
                    QueueBundledTextureAsset(
                        kNinjabrainMessageTextureIds[index],
                        kNinjabrainMessageResourceIds[index]);
                }
            }
            for (auto it = g_state.textureAssets.begin();
                 it != g_state.textureAssets.end();) {
                if (allowedTextureIds.contains(it->first)) {
                    ++it;
                } else {
                    RetireTextureAsset(std::move(it->second));
                    it = g_state.textureAssets.erase(it);
                }
            }
        }
        if (g_showGui.load(std::memory_order_acquire)) {
            QueueBundledTextureAsset(
                kGuiLanguageTextureId, IDR_LANGUAGE_PNG);
            QueueBundledTextureAsset(
                kGuiDiscordTextureId, IDR_DISCORD_PNG);
            QueueBundledTextureAsset(
                kGuiEditorTextureId, IDR_EDITOR_PNG);
        }
    }
    for (auto it = g_state.mirrorSnapshots.begin();
         it != g_state.mirrorSnapshots.end();) {
        const bool stillActive = std::any_of(
            g_state.mirrors.begin(), g_state.mirrors.end(),
            [&](const MirrorConfig& mirror) {
                return !MirrorUsesEveryFrameUpdates(mirror.fps) &&
                       mirror.fps == it->first;
            });
        const bool retainedEyeZoomSnapshot =
            it->first == kEyeZoomSnapshotFpsKey &&
            (g_showEyeZoom.load(std::memory_order_acquire) ||
             g_isTransitioningFromEyeZoom.load(std::memory_order_acquire));
        if (stillActive || retainedEyeZoomSnapshot) {
            ++it;
        } else {
            RetireMirrorSnapshotState(it->second);
            it = g_state.mirrorSnapshots.erase(it);
        }
    }
}

void PollGuiHotkeyFallback() {
    static bool chordWasDown = false;
    const bool lowLevelPressed = ConsumeVulkanGuiHotkeyPress();
    if (!g_state.configSnapshot || !g_state.hwnd ||
        g_state.configSnapshot->guiHotkey.empty()) {
        chordWasDown = false;
        return;
    }

    bool chordDown = true;
    bool chordPressedSinceLastPoll = true;
    for (DWORD key : g_state.configSnapshot->guiHotkey) {
        const SHORT state = GetAsyncKeyState(static_cast<int>(key));
        if ((state & 0x8000) == 0) {
            chordDown = false;
        }
        if ((state & 0x0001) == 0 && (state & 0x8000) == 0) {
            chordPressedSinceLastPoll = false;
        }
    }
    const bool pressed =
        lowLevelPressed ||
        (chordDown && !chordWasDown) ||
        (!chordWasDown && chordPressedSinceLastPoll);
    chordWasDown = chordDown;
    if (!pressed) return;

    const int64_t nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    if (nowMs - g_lastGuiToggleTimeMs.load(std::memory_order_relaxed) <
        200) {
        return;
    }

    ToggleSettingsGuiFromRenderer(g_state.hwnd);
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

void DrawBackgroundImageAsset(
    ImDrawList* draw, const BackgroundConfig& background, TextureAsset& asset,
    float screenW, float screenH, float viewportLeft, float viewportTop,
    float viewportRight, float viewportBottom) {
    TextureFrame* frame = ResolveTextureFrame(asset, true);
    if (!frame || !frame->sampled.linearDescriptor) return;
    const ImTextureID texture = static_cast<ImTextureID>(
        reinterpret_cast<uintptr_t>(frame->sampled.linearDescriptor));
    const ImVec2 uv0(0.0f, 1.0f);
    const ImVec2 uv1(1.0f, 0.0f);
    const std::array<std::array<float, 4>, 4> clips{ {
        { 0.0f, 0.0f, screenW, viewportTop },
        { 0.0f, viewportBottom, screenW, screenH },
        { 0.0f, viewportTop, viewportLeft, viewportBottom },
        { viewportRight, viewportTop, screenW, viewportBottom },
    } };
    const auto drawClipped = [&](const ImVec2& minimum, const ImVec2& maximum) {
        for (const auto& clip : clips) {
            if (clip[2] <= clip[0] || clip[3] <= clip[1]) continue;
            draw->PushClipRect(
                ImVec2(clip[0], clip[1]), ImVec2(clip[2], clip[3]), true);
            draw->AddImage(texture, minimum, maximum, uv0, uv1);
            draw->PopClipRect();
        }
    };

    const BackgroundImageFit fit = ParseBackgroundImageFit(background.imageFit);
    if (fit == BackgroundImageFit::Tile) {
        const std::vector<BackgroundFitRect> tiles = ComputeBackgroundTileRects(
            background.imageTileScale, background.imageTileSpacing, asset.width,
            asset.height, static_cast<int>(screenW), static_cast<int>(screenH));
        for (const BackgroundFitRect& tile : tiles) {
            drawClipped(
                ImVec2(
                    static_cast<float>(tile.left),
                    screenH - static_cast<float>(tile.top)),
                ImVec2(
                    static_cast<float>(tile.right),
                    screenH - static_cast<float>(tile.bottom)));
        }
        return;
    }

    const BackgroundFitRect rect = ResolveBackgroundImageDestRect(
        fit, background.imageCenterScale, asset.width, asset.height,
        static_cast<int>(screenW), static_cast<int>(screenH));
    drawClipped(
        ImVec2(
            static_cast<float>(rect.left),
            screenH - static_cast<float>(rect.top)),
        ImVec2(
            static_cast<float>(rect.right),
            screenH - static_cast<float>(rect.bottom)));
}

void DrawModeBackground(const VulkanRenderer::FinalBlitContext& context, const ModeViewportInfo& viewport) {
    if (!g_state.configSnapshot || !viewport.valid) return;
    const ModeConfig* targetMode =
        GetModeFromSnapshotOrFallback(*g_state.configSnapshot, g_state.modeId);
    if (!targetMode) return;
    const ModeTransitionState transition = GetModeTransitionState();
    const bool transitionEffectivelyComplete =
        transition.active &&
        transition.width == transition.targetWidth &&
        transition.height == transition.targetHeight &&
        transition.x == transition.targetX &&
        transition.y == transition.targetY;
    const bool animationsVisible =
        g_obsCompositionPass ||
        !g_state.configSnapshot->hideAnimationsInGame;
    const bool isAnimating =
        animationsVisible && transition.active &&
        !transitionEffectivelyComplete;
    const ModeConfig* backgroundMode = targetMode;
    const ModeConfig* borderMode = targetMode;
    if (isAnimating && !transition.fromModeId.empty()) {
        const ModeConfig* fromMode = GetModeFromSnapshotOrFallback(
            *g_state.configSnapshot, transition.fromModeId);
        if (fromMode) {
            const bool fromHasSpecialBackground =
                fromMode->background.selectedMode == "gradient" ||
                fromMode->background.selectedMode == "image";
            if (EqualsIgnoreCase(targetMode->id, "Fullscreen") ||
                fromHasSpecialBackground) {
                backgroundMode = fromMode;
            }
            if (EqualsIgnoreCase(targetMode->id, "Fullscreen")) {
                borderMode = fromMode;
            }
        }
    }

    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float screenW = static_cast<float>(context.swapchain->extent.width);
    const float screenH = static_cast<float>(context.swapchain->extent.height);
    const float left = static_cast<float>(viewport.x);
    const float top = static_cast<float>(viewport.y);
    const float right = static_cast<float>(viewport.x + viewport.width);
    const float bottom = static_cast<float>(viewport.y + viewport.height);
    const BackgroundConfig& background = backgroundMode->background;
    const auto drawViewportBorder = [&]() {
        if (!borderMode->border.enabled || borderMode->border.width <= 0) return;
        const Color& border = borderMode->border.color;
        const ImU32 color = ImGui::ColorConvertFloat4ToU32(
            ImVec4(border.r, border.g, border.b, border.a));
        const float inset =
            static_cast<float>(borderMode->border.width) * 0.5f;
        draw->AddRect(
            ImVec2(left + inset, top + inset),
            ImVec2(right - inset, bottom - inset), color,
            static_cast<float>((std::max)(0, borderMode->border.radius)), 0,
            static_cast<float>(borderMode->border.width));
    };

    if (background.selectedMode != "gradient" || background.gradientStops.size() < 2) {
        const ImU32 color = ImGui::ColorConvertFloat4ToU32(
            ImVec4(background.color.r, background.color.g, background.color.b, background.color.a));
        if (top > 0.0f) draw->AddRectFilled(ImVec2(0, 0), ImVec2(screenW, top), color);
        if (bottom < screenH) draw->AddRectFilled(ImVec2(0, bottom), ImVec2(screenW, screenH), color);
        if (left > 0.0f) draw->AddRectFilled(ImVec2(0, top), ImVec2(left, bottom), color);
        if (right < screenW) draw->AddRectFilled(ImVec2(right, top), ImVec2(screenW, bottom), color);
        if (background.selectedMode == "image") {
            auto asset = g_state.textureAssets.find(backgroundMode->id);
            if (asset != g_state.textureAssets.end()) {
                DrawBackgroundImageAsset(
                    draw, background, asset->second, screenW, screenH, left, top,
                    right, bottom);
            }
        }
        drawViewportBorder();
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
    drawViewportBorder();
}

bool PrepareMirrorSource(const VulkanRenderer::FinalBlitContext& context, VkImageLayout& sampleLayout,
                         SampledImage*& sampled) {
    PROFILE_SCOPE_CAT("Vulkan mirror preparation", "Vulkan");
    sampled = nullptr;
    const bool sourceNeeded =
        !g_state.mirrors.empty() ||
        !g_state.slideOutMirrors.empty() ||
        g_showGui.load(std::memory_order_acquire) ||
        g_showEyeZoom.load(std::memory_order_acquire) ||
        g_isTransitioningFromEyeZoom.load(
            std::memory_order_acquire);
    if (!context.sourceMetadata ||
        !sourceNeeded) {
        return false;
    }
    const bool canSampleDirectly =
        (context.sourceMetadata->usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0 &&
        context.sourceMetadata->type == VK_IMAGE_TYPE_2D &&
        context.sourceMetadata->samples == VK_SAMPLE_COUNT_1_BIT &&
        IsFormatSampleable(context.sourceMetadata->format);
    if (canSampleDirectly) {
        sampleLayout = context.sourceLayout == VK_IMAGE_LAYOUT_GENERAL
                           ? VK_IMAGE_LAYOUT_GENERAL
                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        sampled = GetSampledImage(context.sourceImage, *context.sourceMetadata, sampleLayout);
        if (!sampled) return false;

        VkImageMemoryBarrier barrier = MakeBarrier(
            context.sourceImage, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
            context.sourceLayout, sampleLayout);
        g_state.dispatch.cmdPipelineBarrier(
            context.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        return true;
    }

    // Copy and resolve operations only accept TRANSFER_SRC_OPTIMAL or GENERAL
    // for the source. The final-blit source is normally already
    // TRANSFER_SRC_OPTIMAL; do not transition an image that Minecraft still
    // expects in another layout merely to obtain an overlay sample.
    const bool transferSourceLayoutIsLegal =
        context.sourceLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL ||
        context.sourceLayout == VK_IMAGE_LAYOUT_GENERAL;
    if ((context.sourceMetadata->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 ||
        !transferSourceLayoutIsLegal) {
        return false;
    }
    const bool sourceIsMultisampled =
        context.sourceMetadata->samples != VK_SAMPLE_COUNT_1_BIT;
    if ((sourceIsMultisampled && !context.dispatch->cmdResolveImage) ||
        (!sourceIsMultisampled && !g_state.cmdCopyImage)) {
        return false;
    }
    MirrorCopyImage* copy = GetMirrorCopyImage(
        context.sourceImage, *context.sourceMetadata);
    if (!copy) return false;

    VkImageMemoryBarrier toTransfer = MakeBarrier(
        copy->sampled.image,
        copy->initialized ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_NONE,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        copy->initialized ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                          : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    g_state.dispatch.cmdPipelineBarrier(
        context.commandBuffer,
        copy->initialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                          : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toTransfer);

    if (sourceIsMultisampled) {
        VkImageResolve region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.extent = context.sourceMetadata->extent;
        context.dispatch->cmdResolveImage(
            context.commandBuffer, context.sourceImage, context.sourceLayout,
            copy->sampled.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
            &region);
    } else {
        VkImageCopy region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.extent = context.sourceMetadata->extent;
        g_state.cmdCopyImage(
            context.commandBuffer, context.sourceImage, context.sourceLayout,
            copy->sampled.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
            &region);
    }

    VkImageMemoryBarrier toShader = MakeBarrier(
        copy->sampled.image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g_state.dispatch.cmdPipelineBarrier(
        context.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toShader);
    copy->initialized = true;
    sampleLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sampled = &copy->sampled;
    return true;
}

void DrawMirrors(const VulkanRenderer::FinalBlitContext& context, SampledImage* sampled,
                 TimestampFrame* timestampFrame,
                 const MirrorConfig* onlyMirror = nullptr,
                 const std::vector<MirrorConfig>* mirrorList = nullptr,
                 float forcedSlideProgress = -1.0f) {
    PROFILE_SCOPE_CAT("Vulkan mirror rendering", "Vulkan");
    if (!sampled || !context.sourceMetadata) return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float sourceW = static_cast<float>(context.sourceMetadata->extent.width);
    const float sourceH = static_cast<float>(context.sourceMetadata->extent.height);
    ModeViewportInfo viewport = ResolveOverlayViewport(context);
    const int screenW = static_cast<int>(context.swapchain->extent.width);
    const int screenH = static_cast<int>(context.swapchain->extent.height);
    static const auto gradientStart = std::chrono::steady_clock::now();
    const float gradientElapsed =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - gradientStart).count();
    const ModeConfig* activeMode = g_state.configSnapshot
        ? GetModeFromSnapshotOrFallback(*g_state.configSnapshot, g_state.modeId)
        : nullptr;
    const ModeTransitionState transition = GetModeTransitionState();
    const bool animationsVisible =
        g_obsCompositionPass ||
        !g_state.configSnapshot ||
        !g_state.configSnapshot->hideAnimationsInGame;
    float mirrorSlideProgress =
        forcedSlideProgress >= 0.0f
            ? std::clamp(forcedSlideProgress, 0.0f, 1.0f)
            : 1.0f;
    if (forcedSlideProgress < 0.0f && animationsVisible &&
        activeMode && activeMode->slideMirrorsIn && transition.active &&
        transition.gameTransition == GameTransitionType::Bounce &&
        transition.moveProgress < 1.0f) {
        mirrorSlideProgress = std::clamp(transition.moveProgress, 0.0f, 1.0f);
    }
    if (forcedSlideProgress < 0.0f && animationsVisible &&
        g_state.configSnapshot && EqualsIgnoreCase(g_state.modeId, "EyeZoom") &&
        g_state.configSnapshot->eyezoom.slideMirrorsIn) {
        const int targetViewportX = (std::max)(
            0, (screenW - g_state.configSnapshot->eyezoom.windowWidth) / 2);
        const int animatedViewportX =
            g_eyeZoomAnimatedViewportX.load(std::memory_order_acquire);
        if (animatedViewportX >= 0 && targetViewportX > 0 &&
            animatedViewportX < targetViewportX) {
            mirrorSlideProgress = std::clamp(
                static_cast<float>(animatedViewportX) /
                    static_cast<float>(targetViewportX),
                0.0f, 1.0f);
        }
    }

    auto resolveOutputRect = [&](const MirrorConfig& mirror,
                                 const MirrorCaptureConfig& input,
                                 int& outputX, int& outputY,
                                 int& outputW, int& outputH) {
        int captureX = 0;
        int captureY = 0;
        GetRelativeCoords(input.relativeTo, input.x, input.y,
                          mirror.captureWidth, mirror.captureHeight,
                          static_cast<int>(sourceW), static_cast<int>(sourceH),
                          captureX, captureY);
        captureX = std::clamp(captureX, 0, static_cast<int>(sourceW));
        captureY = std::clamp(captureY, 0, static_cast<int>(sourceH));
        const int captureW = std::clamp(
            mirror.captureWidth, 0, static_cast<int>(sourceW) - captureX);
        const int captureH = std::clamp(
            mirror.captureHeight, 0, static_cast<int>(sourceH) - captureY);
        if (captureW <= 0 || captureH <= 0) return false;

        const float scaleX = mirror.output.separateScale
            ? mirror.output.scaleX
            : mirror.output.scale;
        const float scaleY = mirror.output.separateScale
            ? mirror.output.scaleY
            : mirror.output.scale;
        outputW = (std::max)(1, static_cast<int>(captureW * scaleX));
        outputH = (std::max)(1, static_cast<int>(captureH * scaleY));
        const bool viewportRelative =
            mirror.output.relativeTo.ends_with("Viewport");
        if (viewportRelative && viewport.valid && activeMode &&
            activeMode->relativeStretching) {
            outputW = (std::max)(
                1, static_cast<int>(
                       outputW * static_cast<float>(viewport.width) / sourceW));
            outputH = (std::max)(
                1, static_cast<int>(
                       outputH * static_cast<float>(viewport.height) / sourceH));
        }
        const int anchorW =
            viewportRelative && viewport.valid ? viewport.width : screenW;
        const int anchorH =
            viewportRelative && viewport.valid ? viewport.height : screenH;
        int configuredOutputX = mirror.output.x;
        int configuredOutputY = mirror.output.y;
        if (mirror.output.useRelativePosition && !mirror.runtimeGrouped) {
            configuredOutputX = static_cast<int>(
                mirror.output.relativeX * static_cast<float>(screenW));
            configuredOutputY = static_cast<int>(
                mirror.output.relativeY * static_cast<float>(screenH));
        }
        GetRelativeCoords(
            mirror.output.relativeTo, configuredOutputX, configuredOutputY,
            outputW, outputH, anchorW, anchorH, outputX, outputY);
        if (viewportRelative && viewport.valid) {
            outputX += viewport.x;
            outputY += viewport.y;
        }
        return true;
    };

    struct GroupSlideBounds {
        int minimumX = 0;
        int maximumX = 0;
        bool valid = false;
    };
    auto resolveGroupSlideBounds =
        [&](const std::string& groupName) {
        GroupSlideBounds bounds{};
        const std::vector<MirrorConfig>& mirrors =
            mirrorList ? *mirrorList : g_state.mirrors;
        for (const MirrorConfig& grouped : mirrors) {
            if (!grouped.runtimeGrouped ||
                grouped.runtimeGroupName != groupName ||
                grouped.input.empty()) {
                continue;
            }
            int x = 0;
            int y = 0;
            int width = 0;
            int height = 0;
            if (!resolveOutputRect(
                    grouped, grouped.input.front(), x, y, width, height)) {
                continue;
            }
            if (!bounds.valid) {
                bounds.minimumX = x;
                bounds.maximumX = x + width;
                bounds.valid = true;
            } else {
                bounds.minimumX = (std::min)(bounds.minimumX, x);
                bounds.maximumX = (std::max)(bounds.maximumX, x + width);
            }
        }
        return bounds;
    };

    const std::vector<MirrorConfig>& mirrors =
        mirrorList ? *mirrorList : g_state.mirrors;
    for (const MirrorConfig& mirror : mirrors) {
        if (onlyMirror && &mirror != onlyMirror) continue;
        if (g_obsCompositionPass && mirror.onlyOnMyScreen) continue;
        if (mirror.input.empty() || mirror.opacity <= 0.0f) continue;
        const uint64_t mirrorKey = HashMirrorIdentity(mirror.name);
        SampledImage* mirrorSample = nullptr;
        if (g_obsCompositionPass) {
            const auto resolved =
                g_state.frameResolvedMirrorSamples.find(mirror.name);
            if (resolved != g_state.frameResolvedMirrorSamples.end()) {
                mirrorSample = resolved->second;
            }
        }
        if (!mirrorSample) {
            mirrorSample =
                ResolveMirrorSample(mirror, sampled, timestampFrame);
            if (!g_obsCompositionPass) {
                g_state.frameResolvedMirrorSamples[mirror.name] =
                    mirrorSample;
            }
        }
        if (!mirrorSample || !mirrorSample->descriptor) continue;
        bool hasFrameContent = mirror.rawOutput;
        if (!mirror.rawOutput) {
            const auto content = g_state.mirrorHasContent.find(mirrorKey);
            hasFrameContent = content != g_state.mirrorHasContent.end() && content->second;
        }
        for (size_t inputIndex = 0; inputIndex < mirror.input.size(); ++inputIndex) {
        const MirrorCaptureConfig& input = mirror.input[inputIndex];
        int captureX = 0;
        int captureY = 0;
        GetRelativeCoords(input.relativeTo, input.x, input.y, mirror.captureWidth, mirror.captureHeight,
                          static_cast<int>(sourceW), static_cast<int>(sourceH), captureX, captureY);
        captureX = std::clamp(captureX, 0, static_cast<int>(sourceW));
        captureY = std::clamp(captureY, 0, static_cast<int>(sourceH));
        const int captureW = std::clamp(mirror.captureWidth, 0, static_cast<int>(sourceW) - captureX);
        const int captureH = std::clamp(mirror.captureHeight, 0, static_cast<int>(sourceH) - captureY);
        if (captureW <= 0 || captureH <= 0) continue;

        int outputW = 0;
        int outputH = 0;
        int outputX = 0;
        int outputY = 0;
        if (!resolveOutputRect(
                mirror, input, outputX, outputY, outputW, outputH)) {
            continue;
        }
        if (mirrorSlideProgress < 1.0f) {
            const GroupSlideBounds groupedBounds =
                mirror.runtimeGrouped && !mirror.runtimeGroupName.empty()
                    ? resolveGroupSlideBounds(mirror.runtimeGroupName)
                    : GroupSlideBounds{};
            if (groupedBounds.valid) {
                const int groupMinimumX = groupedBounds.minimumX;
                const int groupWidth = (std::max)(
                    1, groupedBounds.maximumX - groupMinimumX);
                const bool leftSide =
                    groupMinimumX + groupWidth / 2 < screenW / 2;
                const int slideX = leftSide
                    ? -groupWidth + static_cast<int>(
                          (groupMinimumX + groupWidth) * mirrorSlideProgress)
                    : screenW - static_cast<int>(
                          (screenW - groupMinimumX) * mirrorSlideProgress);
                outputX += slideX - groupMinimumX;
            } else if (outputX + outputW / 2 < screenW / 2) {
                outputX = -outputW + static_cast<int>(
                    (outputX + outputW) * mirrorSlideProgress);
            } else {
                outputX = screenW - static_cast<int>(
                    (screenW - outputX) * mirrorSlideProgress);
            }
        }
        if (!g_obsCompositionPass) {
            PublishNativeMirrorGeometry(
                mirror.name, captureW, captureH, outputX, outputY,
                outputW, outputH, hasFrameContent);
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
            static_cast<ImTextureID>(
                reinterpret_cast<uintptr_t>(mirrorSample->descriptor));
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
                    ? std::clamp(
                          mirror.border.dynamicThickness, 0,
                          (std::max)(outputW, outputH))
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
            if (mirror.gradientOutput && mirror.gradient.gradientStops.size() >= 2) {
                specialization.gradientStopCount = static_cast<int32_t>(
                    (std::min)(mirror.gradient.gradientStops.size(), size_t{ 8 }));
                for (int32_t i = 0; i < specialization.gradientStopCount; ++i) {
                    const GradientColorStop& stop =
                        mirror.gradient.gradientStops[static_cast<size_t>(i)];
                    specialization.gradientColors[i][0] = stop.color.r;
                    specialization.gradientColors[i][1] = stop.color.g;
                    specialization.gradientColors[i][2] = stop.color.b;
                    specialization.gradientColors[i][3] = stop.color.a;
                    specialization.gradientPositions[i] = stop.position;
                }
                specialization.gradientAngle =
                    mirror.gradient.gradientAngle * 3.14159265358979323846f / 180.0f;
                specialization.gradientAnimationType =
                    static_cast<int32_t>(mirror.gradient.gradientAnimation);
                specialization.gradientAnimationSpeed =
                    mirror.gradient.gradientAnimationSpeed;
                specialization.gradientColorFade =
                    mirror.gradient.gradientColorFade ? 1 : 0;
                specialization.output[3] =
                    std::clamp(mirror.opacity, 0.0f, 1.0f);
            }

            const VkPipeline pipeline = GetMirrorPipeline(specialization);
            if (pipeline) {
                draw->AddCallback(BindMirrorPipelineCallback,
                                  reinterpret_cast<void*>(pipeline));
                const float nonnegativeGradientTime =
                    gradientElapsed > 0.0f ? gradientElapsed : 0.0f;
                const uintptr_t encodedGradientTime =
                    static_cast<uintptr_t>(nonnegativeGradientTime * 1000.0f) + 1u;
                draw->AddCallback(
                    PushMirrorGradientTimeCallback,
                    reinterpret_cast<void*>(encodedGradientTime));
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
                    draw->AddImage(
                        texture, outputMinimum, outputMaximum, uv0, uv1,
                        IM_COL32_WHITE);
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
        if (inputIndex + 1 == mirror.input.size() &&
            border.type == MirrorBorderType::Static && border.staticThickness > 0 &&
            hasFrameContent) {
            const Color& color = border.staticColor;
            const int baseWidth =
                border.staticWidth > 0 ? border.staticWidth : outputW;
            const int baseHeight =
                border.staticHeight > 0 ? border.staticHeight : outputH;
            const int extension = border.staticThickness + 1;
            const int quadWidth = baseWidth + extension * 2;
            const int quadHeight = baseHeight + extension * 2;
            const int centerOffsetX = (baseWidth - outputW) / 2;
            const int centerOffsetY = (baseHeight - outputH) / 2;
            const int quadX = outputX - centerOffsetX +
                              border.staticOffsetX - extension;
            const int quadY = outputY - centerOffsetY +
                              border.staticOffsetY - extension;

            MirrorSpecialization borderSpecialization{};
            borderSpecialization.staticBorderMode = 1;
            borderSpecialization.staticBorderShape =
                static_cast<int32_t>(border.staticShape);
            const VkPipeline borderPipeline =
                GetMirrorPipeline(borderSpecialization);
            if (borderPipeline) {
                MirrorFragmentPushConstants push{};
                push.staticBorderColor[0] = color.r;
                push.staticBorderColor[1] = color.g;
                push.staticBorderColor[2] = color.b;
                push.staticBorderColor[3] =
                    color.a * std::clamp(mirror.opacity, 0.0f, 1.0f);
                push.staticBorderThickness =
                    static_cast<float>(border.staticThickness);
                push.staticBorderRadius =
                    static_cast<float>(border.staticRadius);
                push.staticBorderSize[0] = static_cast<float>(baseWidth);
                push.staticBorderSize[1] = static_cast<float>(baseHeight);
                push.staticBorderQuadSize[0] =
                    static_cast<float>(quadWidth);
                push.staticBorderQuadSize[1] =
                    static_cast<float>(quadHeight);
                g_state.mirrorFragmentPushData.push_back(push);
                draw->AddCallback(
                    BindMirrorPipelineCallback,
                    reinterpret_cast<void*>(borderPipeline));
                draw->AddCallback(
                    PushMirrorFragmentDataCallback,
                    &g_state.mirrorFragmentPushData.back());
                draw->AddImage(
                    texture,
                    ImVec2(static_cast<float>(quadX),
                           static_cast<float>(quadY)),
                    ImVec2(static_cast<float>(quadX + quadWidth),
                           static_cast<float>(quadY + quadHeight)),
                    ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                    IM_COL32_WHITE);
                draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
            }
        }
        }
    }
}

void DrawImages(
    const VulkanRenderer::FinalBlitContext& context,
    const ImageConfig* onlyImage = nullptr) {
    PROFILE_SCOPE_CAT("Vulkan image overlay rendering", "Vulkan");
    if (g_state.images.empty()) return;
    const int screenW = static_cast<int>(context.swapchain->extent.width);
    const int screenH = static_cast<int>(context.swapchain->extent.height);
    const ModeViewportInfo viewport = ResolveOverlayViewport(context);
    const ModeConfig* mode = g_state.configSnapshot
        ? GetModeFromSnapshotOrFallback(*g_state.configSnapshot, g_state.modeId)
        : nullptr;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    for (const ImageConfig& image : g_state.images) {
        if (onlyImage && &image != onlyImage) continue;
        if (g_obsCompositionPass && image.onlyOnMyScreen) continue;
        auto assetIt = g_state.textureAssets.find(image.name);
        if (assetIt == g_state.textureAssets.end()) continue;
        TextureAsset& asset = assetIt->second;
        if (!g_obsCompositionPass) {
            PublishNativeImageSourceDimensions(
                image.name, asset.width, asset.height);
        }
        TextureFrame* frame = ResolveTextureFrame(asset, !image.pixelatedScaling);
        if (!frame) continue;

        const ResolvedCrop crop = ResolveCrop(
            image.crop_top, image.crop_bottom, image.crop_left, image.crop_right,
            image.cropToWidth, image.cropToHeight, asset.width, asset.height);
        const int croppedW =
            (std::max)(1, asset.width - crop.left - crop.right);
        const int croppedH =
            (std::max)(1, asset.height - crop.top - crop.bottom);
        const bool useManualSize =
            !image.relativeSizing && image.width > 0 && image.height > 0;
        int outputW = useManualSize
            ? image.width
            : (std::max)(1, static_cast<int>(croppedW * image.scale));
        int outputH = useManualSize
            ? image.height
            : (std::max)(1, static_cast<int>(croppedH * image.scale));

        const bool viewportRelative = image.relativeTo.ends_with("Viewport");
        if (viewportRelative && viewport.valid && mode && mode->relativeStretching) {
            const int referenceW = context.sourceMetadata
                ? static_cast<int>(context.sourceMetadata->extent.width)
                : mode->width;
            const int referenceH = context.sourceMetadata
                ? static_cast<int>(context.sourceMetadata->extent.height)
                : mode->height;
            const float scaleX = referenceW > 0
                ? static_cast<float>(viewport.width) /
                    static_cast<float>(referenceW)
                : 1.0f;
            const float scaleY = referenceH > 0
                ? static_cast<float>(viewport.height) /
                    static_cast<float>(referenceH)
                : 1.0f;
            outputW = (std::max)(1, static_cast<int>(outputW * scaleX));
            outputH = (std::max)(1, static_cast<int>(outputH * scaleY));
        }

        const int anchorW = viewportRelative && viewport.valid
            ? viewport.width
            : screenW;
        const int anchorH = viewportRelative && viewport.valid
            ? viewport.height
            : screenH;
        int outputX = 0;
        int outputY = 0;
        GetRelativeCoords(
            image.relativeTo, image.x, image.y, outputW, outputH, anchorW, anchorH,
            outputX, outputY);
        if (viewportRelative && viewport.valid) {
            outputX += viewport.x;
            outputY += viewport.y;
        }

        const ImVec2 minimum(
            static_cast<float>(outputX), static_cast<float>(outputY));
        const ImVec2 maximum(
            static_cast<float>(outputX + outputW),
            static_cast<float>(outputY + outputH));
        if (!asset.isFullyTransparent && image.background.enabled &&
            image.background.opacity > 0.0f) {
            const Color& background = image.background.color;
            draw->AddRectFilled(
                minimum, maximum,
                ImGui::ColorConvertFloat4ToU32(ImVec4(
                    background.r, background.g, background.b,
                    background.a * image.background.opacity)));
        }

        VkDescriptorSet descriptor = image.pixelatedScaling
            ? frame->sampled.descriptor
            : frame->sampled.linearDescriptor;
        if (!descriptor) continue;
        const ImTextureID texture = static_cast<ImTextureID>(
            reinterpret_cast<uintptr_t>(descriptor));
        const ImVec2 uv0(
            static_cast<float>(crop.left) / static_cast<float>(asset.width),
            static_cast<float>(asset.height - crop.top) /
                static_cast<float>(asset.height));
        const ImVec2 uv1(
            static_cast<float>(asset.width - crop.right) /
                static_cast<float>(asset.width),
            static_cast<float>(crop.bottom) / static_cast<float>(asset.height));
        const ImU32 tint = IM_COL32(
            255, 255, 255,
            static_cast<int>(
                std::clamp(image.opacity, 0.0f, 1.0f) * 255.0f));
        const float rounding = image.border.enabled
            ? static_cast<float>((std::max)(0, image.border.radius))
            : 0.0f;
        const VkPipeline colorKeyPipeline =
            GetColorKeyPipeline(image.enableColorKey, image.colorKeys);
        if (colorKeyPipeline) {
            draw->AddCallback(
                BindMirrorPipelineCallback,
                reinterpret_cast<void*>(colorKeyPipeline));
        }
        if (rounding > 0.0f) {
            draw->AddImageRounded(
                texture, minimum, maximum, uv0, uv1, tint, rounding);
        } else {
            draw->AddImage(texture, minimum, maximum, uv0, uv1, tint);
        }
        if (colorKeyPipeline) {
            draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        }

        if (!asset.isFullyTransparent && image.border.enabled &&
            image.border.width > 0) {
            const Color& border = image.border.color;
            draw->AddRect(
                minimum, maximum,
                ImGui::ColorConvertFloat4ToU32(
                    ImVec4(border.r, border.g, border.b, border.a)),
                rounding, 0, static_cast<float>(image.border.width));
        }
    }
}

void DrawBrowserOverlays(
    const VulkanRenderer::FinalBlitContext& context,
    const BrowserOverlayConfig* onlyOverlay = nullptr) {
    PROFILE_SCOPE_CAT("Vulkan browser overlay preparation and rendering", "Vulkan");
    if (g_state.browserOverlays.empty()) return;
    const int screenW = static_cast<int>(context.swapchain->extent.width);
    const int screenH = static_cast<int>(context.swapchain->extent.height);
    const ModeViewportInfo viewport = ResolveOverlayViewport(context);
    const ModeConfig* mode = g_state.configSnapshot
        ? GetModeFromSnapshotOrFallback(*g_state.configSnapshot, g_state.modeId)
        : nullptr;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    for (const BrowserOverlayConfig* config : g_state.browserOverlays) {
        if (onlyOverlay && config != onlyOverlay) continue;
        if (!config) continue;
        if (g_obsCompositionPass && config->onlyOnMyScreen) continue;
        BrowserOverlayPixelFrame pixels;
        if (!AcquireBrowserOverlayPixelFrame(*config, pixels) || !pixels.pixels ||
            pixels.pixels->empty()) {
            continue;
        }
        TextureFrame* frame = PrepareStreamingTexture(
            "browser:" + config->name, context.destinationImage,
            pixels.pixels->data(), pixels.width, pixels.height, pixels.generation,
            context.commandBuffer, !config->pixelatedScaling);
        if (!frame) continue;

        const ResolvedCrop crop = ResolveCrop(
            config->crop_top, config->crop_bottom, config->crop_left,
            config->crop_right, config->cropToWidth, config->cropToHeight,
            pixels.width, pixels.height);
        const int croppedW =
            (std::max)(1, pixels.width - crop.left - crop.right);
        const int croppedH =
            (std::max)(1, pixels.height - crop.top - crop.bottom);
        int outputW =
            (std::max)(1, static_cast<int>(croppedW * config->scale));
        int outputH =
            (std::max)(1, static_cast<int>(croppedH * config->scale));
        const bool viewportRelative = config->relativeTo.ends_with("Viewport");
        if (viewportRelative && viewport.valid && mode && mode->relativeStretching) {
            const int referenceW = context.sourceMetadata
                ? static_cast<int>(context.sourceMetadata->extent.width)
                : mode->width;
            const int referenceH = context.sourceMetadata
                ? static_cast<int>(context.sourceMetadata->extent.height)
                : mode->height;
            const float scaleX = referenceW > 0
                ? static_cast<float>(viewport.width) /
                    static_cast<float>(referenceW)
                : 1.0f;
            const float scaleY = referenceH > 0
                ? static_cast<float>(viewport.height) /
                    static_cast<float>(referenceH)
                : 1.0f;
            outputW = (std::max)(1, static_cast<int>(outputW * scaleX));
            outputH = (std::max)(1, static_cast<int>(outputH * scaleY));
        }
        const int anchorW =
            viewportRelative && viewport.valid ? viewport.width : screenW;
        const int anchorH =
            viewportRelative && viewport.valid ? viewport.height : screenH;
        int outputX = 0;
        int outputY = 0;
        GetRelativeCoords(
            config->relativeTo, config->x, config->y, outputW, outputH, anchorW,
            anchorH, outputX, outputY);
        if (viewportRelative && viewport.valid) {
            outputX += viewport.x;
            outputY += viewport.y;
        }
        const ImVec2 minimum(
            static_cast<float>(outputX), static_cast<float>(outputY));
        const ImVec2 maximum(
            static_cast<float>(outputX + outputW),
            static_cast<float>(outputY + outputH));
        if (config->background.enabled && config->background.opacity > 0.0f) {
            const Color& background = config->background.color;
            draw->AddRectFilled(
                minimum, maximum,
                ImGui::ColorConvertFloat4ToU32(ImVec4(
                    background.r, background.g, background.b,
                    background.a * config->background.opacity)));
        }
        const VkDescriptorSet descriptor = config->pixelatedScaling
            ? frame->sampled.descriptor
            : frame->sampled.linearDescriptor;
        if (!descriptor) continue;
        const ImTextureID texture = static_cast<ImTextureID>(
            reinterpret_cast<uintptr_t>(descriptor));
        const ImVec2 uv0(
            static_cast<float>(crop.left) / static_cast<float>(pixels.width),
            static_cast<float>(crop.top) / static_cast<float>(pixels.height));
        const ImVec2 uv1(
            static_cast<float>(pixels.width - crop.right) /
                static_cast<float>(pixels.width),
            static_cast<float>(pixels.height - crop.bottom) /
                static_cast<float>(pixels.height));
        const ImU32 tint = IM_COL32(
            255, 255, 255,
            static_cast<int>(
                std::clamp(config->opacity, 0.0f, 1.0f) * 255.0f));
        const float rounding = config->border.enabled
            ? static_cast<float>((std::max)(0, config->border.radius))
            : 0.0f;
        const VkPipeline colorKeyPipeline =
            GetColorKeyPipeline(config->enableColorKey, config->colorKeys);
        if (colorKeyPipeline) {
            draw->AddCallback(
                BindMirrorPipelineCallback,
                reinterpret_cast<void*>(colorKeyPipeline));
        }
        if (rounding > 0.0f) {
            draw->AddImageRounded(
                texture, minimum, maximum, uv0, uv1, tint, rounding);
        } else {
            draw->AddImage(texture, minimum, maximum, uv0, uv1, tint);
        }
        if (colorKeyPipeline) {
            draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        }
        if (config->border.enabled && config->border.width > 0) {
            const Color& border = config->border.color;
            draw->AddRect(
                minimum, maximum,
                ImGui::ColorConvertFloat4ToU32(
                    ImVec4(border.r, border.g, border.b, border.a)),
                rounding, 0, static_cast<float>(config->border.width));
        }
    }
}

void DrawWindowOverlays(
    const VulkanRenderer::FinalBlitContext& context,
    const WindowOverlayConfig* onlyOverlay = nullptr) {
    PROFILE_SCOPE_CAT("Vulkan window overlay preparation and rendering", "Vulkan");
    if (g_state.windowOverlays.empty()) return;
    const int screenW = static_cast<int>(context.swapchain->extent.width);
    const int screenH = static_cast<int>(context.swapchain->extent.height);
    const ModeViewportInfo viewport = ResolveOverlayViewport(context);
    const ModeConfig* mode = g_state.configSnapshot
        ? GetModeFromSnapshotOrFallback(*g_state.configSnapshot, g_state.modeId)
        : nullptr;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    for (const WindowOverlayConfig* config : g_state.windowOverlays) {
        if (onlyOverlay && config != onlyOverlay) continue;
        if (!config) continue;
        if (g_obsCompositionPass && config->onlyOnMyScreen) continue;
        WindowOverlayPixelFrame pixels;
        if (!AcquireWindowOverlayPixelFrame(*config, pixels) || !pixels.pixels ||
            pixels.pixels->empty()) {
            continue;
        }
        TextureFrame* frame = PrepareStreamingTexture(
            "window:" + config->name, context.destinationImage,
            pixels.pixels->data(), pixels.width, pixels.height, pixels.generation,
            context.commandBuffer, !config->pixelatedScaling);
        if (!frame) continue;
        const ResolvedCrop crop = ResolveCrop(
            config->crop_top, config->crop_bottom, config->crop_left,
            config->crop_right, config->cropToWidth, config->cropToHeight,
            pixels.width, pixels.height);
        const int croppedW =
            (std::max)(1, pixels.width - crop.left - crop.right);
        const int croppedH =
            (std::max)(1, pixels.height - crop.top - crop.bottom);
        const float scaleX =
            config->separateScale ? config->scaleX : config->scale;
        const float scaleY =
            config->separateScale ? config->scaleY : config->scale;
        int outputW = (std::max)(1, static_cast<int>(croppedW * scaleX));
        int outputH = (std::max)(1, static_cast<int>(croppedH * scaleY));
        const bool viewportRelative = config->relativeTo.ends_with("Viewport");
        if (viewportRelative && viewport.valid && mode && mode->relativeStretching) {
            const int referenceW = context.sourceMetadata
                ? static_cast<int>(context.sourceMetadata->extent.width)
                : mode->width;
            const int referenceH = context.sourceMetadata
                ? static_cast<int>(context.sourceMetadata->extent.height)
                : mode->height;
            const float viewportScaleX = referenceW > 0
                ? static_cast<float>(viewport.width) /
                    static_cast<float>(referenceW)
                : 1.0f;
            const float viewportScaleY = referenceH > 0
                ? static_cast<float>(viewport.height) /
                    static_cast<float>(referenceH)
                : 1.0f;
            outputW =
                (std::max)(1, static_cast<int>(outputW * viewportScaleX));
            outputH =
                (std::max)(1, static_cast<int>(outputH * viewportScaleY));
        }
        const int anchorW =
            viewportRelative && viewport.valid ? viewport.width : screenW;
        const int anchorH =
            viewportRelative && viewport.valid ? viewport.height : screenH;
        int outputX = 0;
        int outputY = 0;
        GetRelativeCoords(
            config->relativeTo, config->x, config->y, outputW, outputH, anchorW,
            anchorH, outputX, outputY);
        if (viewportRelative && viewport.valid) {
            outputX += viewport.x;
            outputY += viewport.y;
        }
        const ImVec2 minimum(
            static_cast<float>(outputX), static_cast<float>(outputY));
        const ImVec2 maximum(
            static_cast<float>(outputX + outputW),
            static_cast<float>(outputY + outputH));
        if (config->background.enabled && config->background.opacity > 0.0f) {
            const Color& background = config->background.color;
            draw->AddRectFilled(
                minimum, maximum,
                ImGui::ColorConvertFloat4ToU32(ImVec4(
                    background.r, background.g, background.b,
                    background.a * config->background.opacity)));
        }
        const VkDescriptorSet descriptor = config->pixelatedScaling
            ? frame->sampled.descriptor
            : frame->sampled.linearDescriptor;
        if (!descriptor) continue;
        const ImTextureID texture = static_cast<ImTextureID>(
            reinterpret_cast<uintptr_t>(descriptor));
        const ImVec2 uv0(
            static_cast<float>(crop.left) / static_cast<float>(pixels.width),
            static_cast<float>(crop.top) / static_cast<float>(pixels.height));
        const ImVec2 uv1(
            static_cast<float>(pixels.width - crop.right) /
                static_cast<float>(pixels.width),
            static_cast<float>(pixels.height - crop.bottom) /
                static_cast<float>(pixels.height));
        const ImU32 tint = IM_COL32(
            255, 255, 255,
            static_cast<int>(
                std::clamp(config->opacity, 0.0f, 1.0f) * 255.0f));
        const float rounding = config->border.enabled
            ? static_cast<float>((std::max)(0, config->border.radius))
            : 0.0f;
        const VkPipeline colorKeyPipeline =
            GetColorKeyPipeline(config->enableColorKey, config->colorKeys);
        if (colorKeyPipeline) {
            draw->AddCallback(
                BindMirrorPipelineCallback,
                reinterpret_cast<void*>(colorKeyPipeline));
        }
        if (rounding > 0.0f) {
            draw->AddImageRounded(
                texture, minimum, maximum, uv0, uv1, tint, rounding);
        } else {
            draw->AddImage(texture, minimum, maximum, uv0, uv1, tint);
        }
        if (colorKeyPipeline) {
            draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        }
        if (config->border.enabled && config->border.width > 0) {
            const Color& border = config->border.color;
            draw->AddRect(
                minimum, maximum,
                ImGui::ColorConvertFloat4ToU32(
                    ImVec4(border.r, border.g, border.b, border.a)),
                rounding, 0, static_cast<float>(config->border.width));
        }
    }
}

void DrawEyeZoom(
    const VulkanRenderer::FinalBlitContext& context, SampledImage* source) {
    PROFILE_SCOPE_CAT("Vulkan EyeZoom", "Vulkan");
    const bool showEyeZoom =
        g_showEyeZoom.load(std::memory_order_acquire);
    const bool animationsVisible =
        g_obsCompositionPass ||
        !g_state.configSnapshot ||
        !g_state.configSnapshot->hideAnimationsInGame;
    const bool transitioningFromEyeZoom =
        animationsVisible &&
        g_isTransitioningFromEyeZoom.load(std::memory_order_acquire);
    if (!source || !context.sourceMetadata || !g_state.configSnapshot ||
        (!showEyeZoom && !transitioningFromEyeZoom)) {
        return;
    }
    const EyeZoomConfig& zoom = g_state.configSnapshot->eyezoom;
    const int screenW = static_cast<int>(context.swapchain->extent.width);
    const int screenH = static_cast<int>(context.swapchain->extent.height);
    const int sourceW = static_cast<int>(context.sourceMetadata->extent.width);
    const int sourceH = static_cast<int>(context.sourceMetadata->extent.height);
    const int targetViewportX =
        (std::max)(0, (screenW - zoom.windowWidth) / 2);
    int outputW = 0;
    int outputH = 0;
    int finalX = 0;
    int finalY = 0;
    if (zoom.useCustomSizePosition) {
        outputW = zoom.zoomAreaWidth;
        outputH = zoom.zoomAreaHeight;
        finalX = zoom.positionX;
        finalY = zoom.positionY;
    } else {
        const int horizontalMargin = targetViewportX > 0
            ? targetViewportX / 10
            : 0;
        const int verticalMargin = screenH / 8;
        outputW = targetViewportX - horizontalMargin * 2;
        outputH = screenH - verticalMargin * 2;
        finalX = horizontalMargin;
        finalY = verticalMargin;
    }
    outputW = std::clamp(outputW, 1, (std::max)(1, screenW));
    outputH = std::clamp(outputH, 1, (std::max)(1, screenH));
    finalX = std::clamp(finalX, 0, (std::max)(0, screenW - outputW));
    finalY = std::clamp(finalY, 0, (std::max)(0, screenH - outputH));

    int outputX = finalX;
    const int animatedViewportX = animationsVisible
        ? g_eyeZoomAnimatedViewportX.load(std::memory_order_acquire)
        : -1;
    if (zoom.slideZoomIn && animatedViewportX >= 0 && targetViewportX > 0) {
        const float progress = std::clamp(
            static_cast<float>(animatedViewportX) /
                static_cast<float>(targetViewportX),
            0.0f, 1.0f);
        outputX = -outputW +
            static_cast<int>((finalX + outputW) * progress);
    }
    const float opacity = std::clamp(
        g_eyeZoomFadeOpacity.load(std::memory_order_acquire), 0.0f, 1.0f);
    if (opacity <= 0.0f) return;

    const int cloneW = std::clamp(zoom.cloneWidth, 1, sourceW);
    const int cloneH = std::clamp(zoom.cloneHeight, 1, sourceH);
    const int captureX = (sourceW - cloneW) / 2;
    const int captureY = (sourceH - cloneH) / 2;
    const ImVec2 minimum(
        static_cast<float>(outputX), static_cast<float>(finalY));
    const ImVec2 maximum(
        static_cast<float>(outputX + outputW),
        static_cast<float>(finalY + outputH));
    const ImVec2 uv0(
        static_cast<float>(captureX) / static_cast<float>(sourceW),
        1.0f - static_cast<float>(captureY) / static_cast<float>(sourceH));
    const ImVec2 uv1(
        static_cast<float>(captureX + cloneW) / static_cast<float>(sourceW),
        1.0f -
            static_cast<float>(captureY + cloneH) / static_cast<float>(sourceH));
    const ImTextureID texture = static_cast<ImTextureID>(
        reinterpret_cast<uintptr_t>(source->descriptor));
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->AddImage(
        texture, minimum, maximum, uv0, uv1,
        IM_COL32(255, 255, 255, static_cast<int>(opacity * 255.0f)));

    draw->PushClipRect(minimum, maximum, true);
    bool customOverlayDrawn = false;
    if (zoom.activeOverlayIndex >= 0 &&
        zoom.activeOverlayIndex < static_cast<int>(zoom.overlays.size())) {
        const EyeZoomOverlayConfig& overlay =
            zoom.overlays[static_cast<size_t>(zoom.activeOverlayIndex)];
        auto assetIt =
            g_state.textureAssets.find("ezoverlay_" + overlay.name);
        if (assetIt != g_state.textureAssets.end()) {
            TextureAsset& asset = assetIt->second;
            TextureFrame* frame = ResolveTextureFrame(asset, true);
            if (frame && frame->sampled.linearDescriptor) {
                int overlayW = outputW;
                int overlayH = outputH;
                if (overlay.displayMode == EyeZoomOverlayDisplayMode::Manual) {
                    overlayW = (std::max)(1, overlay.manualWidth);
                    overlayH = (std::max)(1, overlay.manualHeight);
                } else if (
                    overlay.displayMode == EyeZoomOverlayDisplayMode::Fit) {
                    const float scale = (std::min)(
                        static_cast<float>(outputW) /
                            static_cast<float>(asset.width),
                        static_cast<float>(outputH) /
                            static_cast<float>(asset.height));
                    overlayW =
                        (std::max)(1, static_cast<int>(asset.width * scale));
                    overlayH =
                        (std::max)(1, static_cast<int>(asset.height * scale));
                }
                const int overlayX = outputX + (outputW - overlayW) / 2;
                const int overlayY = finalY + (outputH - overlayH) / 2;
                const bool clipOverlay =
                    overlay.displayMode != EyeZoomOverlayDisplayMode::Manual ||
                    overlay.clipToZoomArea;
                if (!clipOverlay) draw->PopClipRect();
                draw->AddImage(
                    static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(
                        frame->sampled.linearDescriptor)),
                    ImVec2(
                        static_cast<float>(overlayX),
                        static_cast<float>(overlayY)),
                    ImVec2(
                        static_cast<float>(overlayX + overlayW),
                        static_cast<float>(overlayY + overlayH)),
                    ImVec2(0, 1), ImVec2(1, 0),
                    IM_COL32(
                        255, 255, 255,
                        static_cast<int>(
                            opacity * std::clamp(overlay.opacity, 0.0f, 1.0f) *
                            255.0f)));
                if (!clipOverlay) draw->PushClipRect(minimum, maximum, true);
                customOverlayDrawn = true;
            }
        }
    }

    if (!customOverlayDrawn) {
        const float pixelWidth =
            static_cast<float>(outputW) / static_cast<float>(cloneW);
        const int labelsPerSide = cloneW / 2;
        const int visibleLabels =
            std::clamp(zoom.overlayWidth, 0, labelsPerSide);
        const float centerY = finalY + outputH * 0.5f;
        ImFont* labelFont = GetOverlayTextFont();
        if (!labelFont || !labelFont->IsLoaded()) labelFont = ImGui::GetFont();
        const float requestedFontSize =
            (std::max)(1.0f, GetOverlayTextFontSize());
        const float boxHeight = zoom.linkRectToFont
            ? requestedFontSize * 1.2f
            : static_cast<float>(zoom.rectHeight);
        float sharedAutoFontSize = requestedFontSize;
        if (zoom.fontSizeMode == EyeZoomFontSizeMode::Auto) {
            for (int offset = -visibleLabels; offset <= visibleLabels;
                 ++offset) {
                if (offset == 0) continue;
                const std::string label = std::to_string(std::abs(offset));
                const ImVec2 requestedTextSize = labelFont->CalcTextSizeA(
                    requestedFontSize, FLT_MAX, 0.0f, label.c_str());
                if (requestedTextSize.x <= 0.0f ||
                    requestedTextSize.y <= 0.0f) {
                    continue;
                }
                const float fitScale = (std::min)(
                    (std::min)(
                        pixelWidth * 0.82f / requestedTextSize.x,
                        boxHeight * 0.82f / requestedTextSize.y),
                    1.0f);
                sharedAutoFontSize = (std::max)(
                    1.0f,
                    (std::min)(
                        sharedAutoFontSize,
                        requestedFontSize * fitScale));
            }
        }
        for (int offset = -visibleLabels; offset <= visibleLabels; ++offset) {
            if (offset == 0) continue;
            const int boxIndex =
                offset + labelsPerSide - (offset > 0 ? 1 : 0);
            const float boxLeft = outputX + boxIndex * pixelWidth;
            const ImVec2 boxMin(boxLeft, centerY - boxHeight * 0.5f);
            const ImVec2 boxMax(
                boxLeft + pixelWidth, centerY + boxHeight * 0.5f);
            const bool even = (boxIndex % 2) == 0;
            const Color& color = even ? zoom.gridColor1 : zoom.gridColor2;
            const float alpha = even
                ? zoom.gridColor1Opacity
                : zoom.gridColor2Opacity;
            draw->AddRectFilled(
                boxMin, boxMax,
                ImGui::ColorConvertFloat4ToU32(ImVec4(
                    color.r, color.g, color.b, alpha * opacity)));
            const std::string label = std::to_string(std::abs(offset));
            float fontSize = requestedFontSize;
            if (zoom.fontSizeMode == EyeZoomFontSizeMode::Auto) {
                fontSize = sharedAutoFontSize;
            } else if (
                zoom.fontSizeMode == EyeZoomFontSizeMode::PerSquareAuto) {
                const ImVec2 requestedTextSize = labelFont->CalcTextSizeA(
                    fontSize, FLT_MAX, 0.0f, label.c_str());
                if (requestedTextSize.x > 0.0f &&
                    requestedTextSize.y > 0.0f) {
                    const float fitScale = (std::min)(
                        (std::min)(
                            pixelWidth * 0.82f / requestedTextSize.x,
                            boxHeight * 0.82f / requestedTextSize.y),
                        1.0f);
                    fontSize = (std::max)(1.0f, fontSize * fitScale);
                }
            }
            const ImVec2 textSize = labelFont->CalcTextSizeA(
                fontSize, FLT_MAX, 0.0f, label.c_str());
            draw->AddText(
                labelFont, fontSize,
                ImVec2(
                    boxLeft + (pixelWidth - textSize.x) * 0.5f,
                    centerY - textSize.y * 0.5f),
                ImGui::ColorConvertFloat4ToU32(ImVec4(
                    zoom.textColor.r, zoom.textColor.g, zoom.textColor.b,
                    zoom.textColorOpacity * opacity)),
                label.c_str());
        }
    }
    const float centerX = outputX + outputW * 0.5f;
    draw->AddLine(
        ImVec2(centerX, static_cast<float>(finalY)),
        ImVec2(centerX, static_cast<float>(finalY + outputH)),
        ImGui::ColorConvertFloat4ToU32(ImVec4(
            zoom.centerLineColor.r, zoom.centerLineColor.g,
            zoom.centerLineColor.b, zoom.centerLineColorOpacity * opacity)),
        2.0f);
    draw->PopClipRect();

    const ModeConfig* eyeZoomMode = GetModeFromSnapshotOrFallback(
        *g_state.configSnapshot, "EyeZoom");
    if (eyeZoomMode && eyeZoomMode->border.enabled &&
        eyeZoomMode->border.width > 0) {
        const BorderConfig& border = eyeZoomMode->border;
        const float inset = static_cast<float>(border.width) * 0.5f;
        draw->AddRect(
            ImVec2(minimum.x + inset, minimum.y + inset),
            ImVec2(maximum.x - inset, maximum.y - inset),
            ImGui::ColorConvertFloat4ToU32(ImVec4(
                border.color.r, border.color.g, border.color.b,
                border.color.a * opacity)),
            static_cast<float>((std::max)(0, border.radius)), 0,
            static_cast<float>(border.width));
    }
}

void DrawRebindIndicator(
    const VulkanRenderer::FinalBlitContext& context) {
    if (!g_state.configSnapshot) return;
    const KeyRebindsConfig& rebinds =
        g_state.configSnapshot->keyRebinds;
    const int mode = rebinds.indicatorMode;
    if (mode == 0) return;

    const auto now = std::chrono::steady_clock::now();
    if (rebinds.enabled != g_state.rebindIndicatorPreviousEnabled) {
        g_state.rebindIndicatorPreviousEnabled = rebinds.enabled;
        g_state.rebindIndicatorToggleTime = now;
    }
    constexpr float kFadeDurationSeconds = 0.25f;
    const float elapsed = std::chrono::duration<float>(
        now - g_state.rebindIndicatorToggleTime).count();
    const float progress = elapsed < kFadeDurationSeconds
        ? elapsed / kFadeDurationSeconds
        : 1.0f;
    g_state.rebindIndicatorAlpha =
        rebinds.enabled ? progress : 1.0f - progress;

    const int screenW = static_cast<int>(context.swapchain->extent.width);
    const int screenH = static_cast<int>(context.swapchain->extent.height);
    if (screenW <= 0 || screenH <= 0) return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    const auto drawAsset = [&](const char* id, float opacity) {
        if (opacity <= 0.0f) return;
        auto assetIt = g_state.textureAssets.find(id);
        if (assetIt == g_state.textureAssets.end()) return;
        TextureAsset& asset = assetIt->second;
        TextureFrame* frame = ResolveTextureFrame(asset, true);
        if (!frame || !frame->sampled.linearDescriptor) return;

        const float scale = static_cast<float>(screenH) / 1080.0f;
        const float drawW = static_cast<float>(asset.width) * scale;
        const float drawH = static_cast<float>(asset.height) * scale;
        const float margin = 10.0f * scale;
        float x = margin;
        float y = margin;
        switch (rebinds.indicatorPosition) {
        case 1:
            x = static_cast<float>(screenW) - drawW - margin;
            break;
        case 2:
            y = static_cast<float>(screenH) - drawH - margin;
            break;
        case 3:
        default:
            x = static_cast<float>(screenW) - drawW - margin;
            y = static_cast<float>(screenH) - drawH - margin;
            break;
        }
        draw->AddImage(
            static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(
                frame->sampled.linearDescriptor)),
            ImVec2(x, y), ImVec2(x + drawW, y + drawH),
            ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f),
            IM_COL32(
                255, 255, 255,
                static_cast<int>(
                    std::clamp(opacity, 0.0f, 1.0f) * 255.0f)));
    };

    if (mode == 1) {
        drawAsset(
            kRebindIndicatorEnabledTextureId,
            g_state.rebindIndicatorAlpha);
    } else if (mode == 2) {
        drawAsset(
            kRebindIndicatorDisabledTextureId,
            1.0f - g_state.rebindIndicatorAlpha);
    } else if (mode == 3) {
        drawAsset(
            kRebindIndicatorDisabledTextureId,
            1.0f - g_state.rebindIndicatorAlpha);
        drawAsset(
            kRebindIndicatorEnabledTextureId,
            g_state.rebindIndicatorAlpha);
    }
}

void DrawStartupIndicator(
    const VulkanRenderer::FinalBlitContext& context) {
    if (!g_state.configSnapshot ||
        g_state.configSnapshot->startupIndicatorMode == 0 ||
        g_configurePromptDismissedThisSession.load(std::memory_order_relaxed)) {
        return;
    }

    const bool fullscreen = EqualsIgnoreCase(g_state.modeId, "Fullscreen");
    static bool wasFullscreen = false;
    static bool firstFrame = true;
    static std::chrono::steady_clock::time_point enteredFullscreen{};
    const auto now = std::chrono::steady_clock::now();
    if (fullscreen && (!wasFullscreen || firstFrame)) {
        enteredFullscreen = now;
    }
    wasFullscreen = fullscreen;
    firstFrame = false;
    if (!fullscreen) return;

    constexpr float kHoldSeconds = 10.0f;
    constexpr float kFadeSeconds = 1.5f;
    const float elapsed =
        std::chrono::duration_cast<std::chrono::duration<float>>(
            now - enteredFullscreen)
            .count();
    if (elapsed >= kHoldSeconds + kFadeSeconds) return;
    const float opacity = elapsed <= kHoldSeconds
        ? 1.0f
        : std::clamp(
              1.0f - (elapsed - kHoldSeconds) / kFadeSeconds, 0.0f, 1.0f);

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float screenHeight =
        static_cast<float>(context.swapchain->extent.height);
    if (g_state.configSnapshot->startupIndicatorMode == 2) {
        const auto assetIt =
            g_state.textureAssets.find(kStartupIndicatorTextureId);
        if (assetIt != g_state.textureAssets.end()) {
            TextureAsset& asset = assetIt->second;
            TextureFrame* frame = ResolveTextureFrame(asset, true);
            if (frame && frame->sampled.linearDescriptor &&
                asset.width > 0 && asset.height > 0) {
                const float scale = screenHeight / 1080.0f * 0.45f;
                const ImTextureID texture = static_cast<ImTextureID>(
                    reinterpret_cast<uintptr_t>(
                        frame->sampled.linearDescriptor));
                draw->AddImage(
                    texture, ImVec2(0.0f, 0.0f),
                    ImVec2(
                        static_cast<float>(asset.width) * scale,
                        static_cast<float>(asset.height) * scale),
                    ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                    IM_COL32(
                        255, 255, 255,
                        static_cast<int>(opacity * 255.0f)));
                return;
            }
        }
    }

    const float unit = screenHeight / 1080.0f * 0.45f;
    const ImVec2 size(700.0f * unit, 250.0f * unit);
    const float rounding = 28.0f * unit;
    auto alphaColor = [opacity](ImU32 color) {
        ImVec4 value = ImGui::ColorConvertU32ToFloat4(color);
        value.w *= opacity;
        return ImGui::ColorConvertFloat4ToU32(value);
    };
    draw->AddRectFilled(
        ImVec2(0.0f, 0.0f), size,
        alphaColor(IM_COL32(36, 10, 58, 244)), rounding,
        ImDrawFlags_RoundCornersBottomRight);
    draw->AddRectFilledMultiColor(
        ImVec2(0.0f, 0.0f), size,
        alphaColor(IM_COL32(70, 45, 113, 208)),
        alphaColor(IM_COL32(40, 14, 64, 72)),
        alphaColor(IM_COL32(36, 10, 58, 0)),
        alphaColor(IM_COL32(65, 41, 107, 146)));

    ImFont* font = ImGui::GetFont();
    std::string title = tr("label.toolscreen");
    std::string hotkey =
        GetKeyComboString(g_state.configSnapshot->guiHotkey);
    if (hotkey.empty()) hotkey = tr("hotkeys.none");
    const std::string message =
        tr("welcome_toast.fullscreen_press_to_configure", hotkey);
    auto fittedSize = [&](const std::string& text, float desired,
                          float minimum, float maximumWidth) {
        float fontSize = desired;
        while (fontSize > minimum &&
               font->CalcTextSizeA(
                       fontSize, FLT_MAX, 0.0f, text.c_str())
                       .x > maximumWidth) {
            fontSize -= unit;
        }
        return fontSize;
    };
    auto centeredText = [&](const std::string& text, float centerY,
                            float fontSize, ImU32 color) {
        const ImVec2 measured =
            font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text.c_str());
        draw->AddText(
            font, fontSize,
            ImVec2((size.x - measured.x) * 0.5f,
                   centerY - measured.y * 0.5f),
            alphaColor(color), text.c_str());
    };
    centeredText(
        title, 52.0f * unit,
        fittedSize(title, 66.0f * unit, 34.0f * unit,
                   size.x - 80.0f * unit),
        IM_COL32(243, 224, 151, 255));
    centeredText(
        message, 155.0f * unit,
        fittedSize(message, 48.0f * unit, 22.0f * unit,
                   size.x - 78.0f * unit),
        IM_COL32(247, 241, 224, 255));
}

void DrawCursorVisuals(const VulkanRenderer::FinalBlitContext& context) {
    PROFILE_SCOPE_CAT("Vulkan cursor and trail overlays", "Vulkan");
    if (!g_state.configSnapshot || !g_state.hwnd) return;
    const Config& config = *g_state.configSnapshot;
    const bool fakeCursor =
        g_obsCompositionPass ? config.captureFakeCursor
                             : config.debug.fakeCursor;
    const CursorTrailConfig& trail = config.cursorTrail;
    if ((!fakeCursor && !trail.enabled) || !IsCursorVisible()) return;

    POINT point{};
    if (!GetCursorPos(&point) || !ScreenToClient(g_state.hwnd, &point)) return;
    RECT client{};
    if (!GetClientRect(g_state.hwnd, &client)) return;
    const int clientW = client.right - client.left;
    const int clientH = client.bottom - client.top;
    const int screenW = static_cast<int>(context.swapchain->extent.width);
    const int screenH = static_cast<int>(context.swapchain->extent.height);
    if (clientW <= 0 || clientH <= 0 || screenW <= 0 || screenH <= 0) return;
    const ImVec2 cursor(
        static_cast<float>(point.x) * static_cast<float>(screenW) /
            static_cast<float>(clientW),
        static_cast<float>(point.y) * static_cast<float>(screenH) /
            static_cast<float>(clientH));
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const int64_t nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();

    const bool showTrail =
        trail.enabled &&
        (g_obsCompositionPass ? !trail.onlyOnMyScreen : !trail.onlyOnObs);
    // Advance the trail once on the ordinary Minecraft pass. The OBS pass only
    // renders the completed sample set, so it cannot double the trail velocity,
    // lifetime, or stamp count.
    if (trail.enabled && !g_obsCompositionPass) {
        const float spacing =
            static_cast<float>((std::max)(1, trail.stampSpacingPx));
        const int64_t previousCallMs = g_state.cursorTrailLastCallMs;
        g_state.cursorTrailLastCallMs = nowMs;
        if (previousCallMs > 0 && nowMs > previousCallMs &&
            nowMs - previousCallMs > 100) {
            g_state.cursorTrailSampleCount = 0;
        }
        g_state.cursorTrailSamples[0] = g_state.cursorTrailSamples[1];
        g_state.cursorTrailSamples[1] = g_state.cursorTrailSamples[2];
        g_state.cursorTrailSamples[2] = cursor;
        g_state.cursorTrailSampleCount =
            (std::min)(size_t{ 3 }, g_state.cursorTrailSampleCount + 1);

        if (g_state.cursorTrailSampleCount >= 2) {
            const ImVec2 delta(
                g_state.cursorTrailSamples[2].x -
                    g_state.cursorTrailSamples[1].x,
                g_state.cursorTrailSamples[2].y -
                    g_state.cursorTrailSamples[1].y);
            const float distance =
                std::sqrt(delta.x * delta.x + delta.y * delta.y);
            const float diagonal = std::sqrt(
                static_cast<float>(screenW * screenW + screenH * screenH));
            if (distance > diagonal) {
                g_state.cursorTrailSampleCount = 0;
            }
        }

        const auto appendStamp =
            [&](const ImVec2& position, float sizeBoost) {
            size_t index = 0;
            if (g_state.cursorTrailCount < g_state.cursorTrailPoints.size()) {
                index = (g_state.cursorTrailStart + g_state.cursorTrailCount) %
                    g_state.cursorTrailPoints.size();
                ++g_state.cursorTrailCount;
            } else {
                index = g_state.cursorTrailStart;
                g_state.cursorTrailStart =
                    (g_state.cursorTrailStart + 1) %
                    g_state.cursorTrailPoints.size();
            }
            g_state.cursorTrailPoints[index] = {
                position, nowMs, sizeBoost };
        };

        if (g_state.cursorTrailSampleCount >= 2) {
            const ImVec2 start = g_state.cursorTrailSamples[1];
            const ImVec2 end = g_state.cursorTrailSamples[2];
            const ImVec2 control = g_state.cursorTrailSampleCount >= 3
                ? ImVec2(
                    start.x +
                        (end.x - g_state.cursorTrailSamples[0].x) * 0.25f,
                    start.y +
                        (end.y - g_state.cursorTrailSamples[0].y) * 0.25f)
                : ImVec2(
                    (start.x + end.x) * 0.5f,
                    (start.y + end.y) * 0.5f);
            const ImVec2 chord(end.x - start.x, end.y - start.y);
            const float distance =
                std::sqrt(chord.x * chord.x + chord.y * chord.y);
            const int steps = (std::min)(
                128, static_cast<int>(distance / spacing));
            float sizeBoost = 1.0f;
            if (trail.useVelocitySize && previousCallMs > 0 &&
                nowMs > previousCallMs) {
                const float velocity =
                    distance / static_cast<float>(nowMs - previousCallMs);
                const float fraction =
                    std::clamp(velocity / 2.0f, 0.0f, 1.0f);
                sizeBoost +=
                    std::clamp(
                        trail.velocitySizeIntensity, 0.0f, 1.0f) *
                    fraction;
            }
            for (int step = 1; step <= steps; ++step) {
                const float t =
                    static_cast<float>(step) /
                    static_cast<float>(steps);
                const float inverse = 1.0f - t;
                appendStamp(
                    ImVec2(
                        start.x * (inverse * inverse) +
                            control.x * (2.0f * inverse * t) +
                            end.x * (t * t),
                        start.y * (inverse * inverse) +
                            control.y * (2.0f * inverse * t) +
                            end.y * (t * t)),
                    sizeBoost);
            }
        }

        while (g_state.cursorTrailCount > 0) {
            const CursorTrailPoint& oldest =
                g_state.cursorTrailPoints[g_state.cursorTrailStart];
            if (nowMs - oldest.timeMs <= (std::max)(1, trail.lifetimeMs)) break;
            g_state.cursorTrailStart =
                (g_state.cursorTrailStart + 1) %
                g_state.cursorTrailPoints.size();
            --g_state.cursorTrailCount;
        }
    }
    if (showTrail) {
        ImTextureID trailTexture = 0;
        if (auto assetIt = g_state.textureAssets.find(kCursorTrailTextureId);
            assetIt != g_state.textureAssets.end()) {
            TextureFrame* frame =
                ResolveTextureFrame(assetIt->second, true);
            if (frame && frame->sampled.linearDescriptor) {
                trailTexture = static_cast<ImTextureID>(
                    reinterpret_cast<uintptr_t>(
                        frame->sampled.linearDescriptor));
            }
        }
        VkPipeline trailPipeline = VK_NULL_HANDLE;
        if (trail.blendMode == "Additive") {
            trailPipeline = GetOverlayPipeline(
                OverlaySpecialization{}, OverlayBlendMode::Additive);
            if (trailPipeline) {
                draw->AddCallback(
                    BindMirrorPipelineCallback,
                    reinterpret_cast<void*>(trailPipeline));
            }
        }
        for (size_t i = 0; i < g_state.cursorTrailCount; ++i) {
            const CursorTrailPoint& stamp =
                g_state.cursorTrailPoints[
                    (g_state.cursorTrailStart + i) %
                    g_state.cursorTrailPoints.size()];
            const float age = std::clamp(
                static_cast<float>(nowMs - stamp.timeMs) /
                    static_cast<float>((std::max)(1, trail.lifetimeMs)),
                0.0f, 1.0f);
            const float headAmount = 1.0f - age;
            const Color color = trail.useGradient
                ? Color{
                      trail.tailColor.r +
                          (trail.color.r - trail.tailColor.r) * headAmount,
                      trail.tailColor.g +
                          (trail.color.g - trail.tailColor.g) * headAmount,
                      trail.tailColor.b +
                          (trail.color.b - trail.tailColor.b) * headAmount,
                      1.0f }
                : trail.color;
            const float sizeScale =
                std::clamp(trail.tailSizeScale, 0.0f, 2.0f) +
                (1.0f -
                 std::clamp(trail.tailSizeScale, 0.0f, 2.0f)) *
                    headAmount;
            const float radius =
                static_cast<float>((std::max)(1, trail.spriteSizePx)) *
                sizeScale * stamp.sizeBoost * 0.5f;
            const ImU32 tint = ImGui::ColorConvertFloat4ToU32(ImVec4(
                color.r, color.g, color.b,
                std::clamp(trail.opacity, 0.0f, 1.0f) * headAmount));
            if (trailTexture) {
                draw->AddImage(
                    trailTexture,
                    ImVec2(
                        stamp.position.x - radius,
                        stamp.position.y - radius),
                    ImVec2(
                        stamp.position.x + radius,
                        stamp.position.y + radius),
                    ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f), tint);
            } else {
                draw->AddCircleFilled(stamp.position, radius, tint);
            }
        }
        if (trailPipeline) {
            draw->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
        }
    }
    if (!trail.enabled && !g_obsCompositionPass) {
        g_state.cursorTrailCount = 0;
        g_state.cursorTrailHasLastPosition = false;
        g_state.cursorTrailSampleCount = 0;
        g_state.cursorTrailLastCallMs = 0;
    }

    if (fakeCursor) {
        CURSORINFO cursorInfo{ sizeof(CURSORINFO) };
        const bool haveCursorInfo =
            GetCursorInfo(&cursorInfo) && cursorInfo.hCursor &&
            (cursorInfo.flags & CURSOR_SHOWING);
        if (haveCursorInfo &&
            (g_state.cursorBitmapHandle != cursorInfo.hCursor ||
             g_state.cursorBitmap.rgbaPixels.empty())) {
            CursorTextures::CursorData decoded;
            if (CursorTextures::CopyCursorDataFromHandle(
                    cursorInfo.hCursor, decoded)) {
                g_state.cursorBitmapHandle = cursorInfo.hCursor;
                g_state.cursorBitmap = std::move(decoded);
                ++g_state.cursorBitmapGeneration;
            }
        }

        const CursorTextures::CursorData& bitmap = g_state.cursorBitmap;
        TextureFrame* cursorFrame = nullptr;
        TextureFrame* cursorInvertFrame = nullptr;
        if (g_state.cursorBitmapHandle && !bitmap.rgbaPixels.empty()) {
            cursorFrame = PrepareStreamingTexture(
                "vulkan-fake-cursor", context.destinationImage,
                bitmap.rgbaPixels.data(), bitmap.bitmapWidth,
                bitmap.bitmapHeight, g_state.cursorBitmapGeneration,
                context.commandBuffer, false);
            if (bitmap.hasInvertedPixels &&
                !bitmap.invertRgbaPixels.empty()) {
                cursorInvertFrame = PrepareStreamingTexture(
                    "vulkan-fake-cursor-invert",
                    context.destinationImage,
                    bitmap.invertRgbaPixels.data(), bitmap.bitmapWidth,
                    bitmap.bitmapHeight, g_state.cursorBitmapGeneration,
                    context.commandBuffer, false);
            }
        }
        if (cursorFrame && cursorFrame->sampled.descriptor) {
            const float offset =
                bitmap.loadType == IMAGE_CURSOR ? 1.5f : 1.0f;
            const float width =
                static_cast<float>(bitmap.bitmapWidth) *
                static_cast<float>(screenW) / static_cast<float>(clientW) *
                offset;
            const float height =
                static_cast<float>(bitmap.bitmapHeight) *
                static_cast<float>(screenH) / static_cast<float>(clientH) *
                offset;
            const ImVec2 hotspot(
                static_cast<float>(bitmap.hotspotX) *
                    static_cast<float>(screenW) /
                    static_cast<float>(clientW) * offset,
                static_cast<float>(bitmap.hotspotY) *
                    static_cast<float>(screenH) /
                    static_cast<float>(clientH) * offset);
            const ImVec2 topLeft(
                cursor.x - hotspot.x, cursor.y - hotspot.y);
            draw->AddImage(
                static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(
                    cursorFrame->sampled.descriptor)),
                topLeft, ImVec2(topLeft.x + width, topLeft.y + height),
                ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
            if (cursorInvertFrame &&
                cursorInvertFrame->sampled.descriptor) {
                if (VkPipeline invertPipeline = GetOverlayPipeline(
                        OverlaySpecialization{},
                        OverlayBlendMode::Invert)) {
                    draw->AddCallback(
                        BindMirrorPipelineCallback,
                        reinterpret_cast<void*>(invertPipeline));
                    draw->AddImage(
                        static_cast<ImTextureID>(
                            reinterpret_cast<uintptr_t>(
                                cursorInvertFrame->sampled.descriptor)),
                        topLeft,
                        ImVec2(topLeft.x + width, topLeft.y + height),
                        ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
                    draw->AddCallback(
                        ImDrawCallback_ResetRenderState, nullptr);
                }
            }
        } else {
            const float size = config.cursors.enabled
                ? static_cast<float>(
                      (std::max)(8, config.cursors.ingame.cursorSize))
                : 24.0f;
            const ImVec2 arrow[4] = {
                cursor,
                ImVec2(cursor.x, cursor.y + size),
                ImVec2(cursor.x + size * 0.28f, cursor.y + size * 0.72f),
                ImVec2(cursor.x + size * 0.52f, cursor.y + size * 0.98f),
            };
            draw->AddConvexPolyFilled(arrow, 4, IM_COL32_WHITE);
            draw->AddPolyline(
                arrow, 4, IM_COL32_BLACK, ImDrawFlags_Closed, 1.5f);
        }
    }
}

// The legacy debug grid enumerates arbitrary OpenGL texture IDs.  A Vulkan
// renderer must never query those objects, so provide the equivalent inspector
// for the resources this backend actually owns and can legally sample.
void DrawNativeTextureGrid(const VulkanRenderer::FinalBlitContext& context,
                           SampledImage* mirrorSource) {
    if (!g_state.configSnapshot ||
        !g_state.configSnapshot->debug.showTextureGrid) {
        return;
    }
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    if (!draw) return;

    struct Entry {
        std::string_view name;
        SampledImage* image = nullptr;
        int width = 0;
        int height = 0;
    };
    std::vector<Entry> entries;
    entries.reserve(g_state.textureAssets.size() + 1);
    if (mirrorSource && mirrorSource->descriptor && context.sourceMetadata) {
        entries.push_back({"Minecraft final source", mirrorSource,
                           static_cast<int>(context.sourceMetadata->extent.width),
                           static_cast<int>(context.sourceMetadata->extent.height)});
    }
    for (auto& [name, asset] : g_state.textureAssets) {
        TextureFrame* frame = ResolveTextureFrame(asset, true);
        if (!frame || !frame->sampled.linearDescriptor) continue;
        entries.push_back({name, &frame->sampled, asset.width, asset.height});
    }
    if (entries.empty()) return;

    constexpr float tileSize = 128.0f;
    constexpr float padding = 4.0f;
    constexpr float margin = 8.0f;
    const float displayW = static_cast<float>(context.swapchain->extent.width);
    const int columns = (std::max)(1, static_cast<int>((displayW - margin * 2.0f) / (tileSize + padding)));
    for (size_t index = 0; index < entries.size(); ++index) {
        const Entry& entry = entries[index];
        const int column = static_cast<int>(index) % columns;
        const int row = static_cast<int>(index) / columns;
        const ImVec2 minimum(margin + column * (tileSize + padding),
                             margin + row * (tileSize + 34.0f));
        const ImVec2 maximum(minimum.x + tileSize, minimum.y + tileSize);
        const ImTextureID descriptor = static_cast<ImTextureID>(
            reinterpret_cast<uintptr_t>(entry.image->linearDescriptor
                ? entry.image->linearDescriptor : entry.image->descriptor));
        draw->AddRectFilled(minimum, maximum, IM_COL32(20, 20, 20, 230));
        draw->AddImage(descriptor, minimum, maximum);
        draw->AddRect(minimum, maximum, IM_COL32(255, 255, 255, 190));
        const std::string label = std::string(entry.name) + "  " +
            std::to_string(entry.width) + "x" + std::to_string(entry.height);
        draw->AddText(ImVec2(minimum.x, maximum.y + 2.0f),
                      IM_COL32(255, 255, 255, 255), label.c_str());
    }
}

void GenerateImGui(const VulkanRenderer::FinalBlitContext& context, SampledImage* mirrorSource,
                    SampledImage* eyeZoomSource,
                    TimestampFrame* timestampFrame) {
    PROFILE_SCOPE_CAT("Vulkan ImGui generation", "Vulkan");
    const bool obsPass = g_obsCompositionPass;
    if (!obsPass) {
        g_state.frameResolvedTextureFrames.clear();
        g_state.frameResolvedMirrorSamples.clear();
    }
    g_activeFrameContext = &context;
    // The game-window context is the only input-owning ImGui context.  The OBS
    // context is deliberately passive: it exists solely to build the filtered
    // OBS composition and must not advance Win32/Vulkan backend state or drain
    // the input queue a second time.
    ImGui::SetCurrentContext(obsPass ? g_state.obsImGuiContext : g_state.imguiContext);
    if (!obsPass) {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplWin32_NewFrame();
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(context.swapchain->extent.width),
                            static_cast<float>(context.swapchain->extent.height));
    if (obsPass) {
        // Core ImGui requires a positive delta time even for a draw-only pass.
        io.DeltaTime = 1.0f / 60.0f;
    } else {
        ImGuiInputQueue_DrainToImGui();
    }
    ImGui::NewFrame();
    g_state.mirrorFragmentPushData.clear();
    if (g_state.mirrorFragmentPushData.capacity() < g_state.mirrors.size()) {
        g_state.mirrorFragmentPushData.reserve(g_state.mirrors.size());
    }
    g_state.pickerTextureId = mirrorSource
        ? reinterpret_cast<uintptr_t>(mirrorSource->descriptor)
        : 0;
    g_state.pickerFrameWidth = context.sourceMetadata
        ? static_cast<int>(context.sourceMetadata->extent.width)
        : 0;
    g_state.pickerFrameHeight = context.sourceMetadata
        ? static_cast<int>(context.sourceMetadata->extent.height)
        : 0;

    if (g_configLoadFailed.load(std::memory_order_acquire) &&
        !g_obsCompositionPass) {
        RenderConfigErrorGUI();
    } else {
        DrawModeBackground(context, ResolveSubmittedViewport(context));
        DrawEyeZoom(context, eyeZoomSource);
        const ModeConfig* mode = g_state.configSnapshot
            ? GetModeFromSnapshotOrFallback(
                  *g_state.configSnapshot, g_state.modeId)
            : nullptr;
        bool slideOutMirrorsDrawn = false;
        const auto drawSlideOutMirrors = [&]() {
            if (slideOutMirrorsDrawn ||
                g_state.slideOutMirrors.empty() ||
                !g_state.configSnapshot) {
                return;
            }
            const bool animationsVisible =
                g_obsCompositionPass ||
                !g_state.configSnapshot->hideAnimationsInGame;
            if (!animationsVisible) {
                slideOutMirrorsDrawn = true;
                return;
            }
            const ModeTransitionState transition =
                GetModeTransitionState();
            if (!transition.active ||
                transition.gameTransition !=
                    GameTransitionType::Bounce) {
                slideOutMirrorsDrawn = true;
                return;
            }
            float progress =
                1.0f -
                std::clamp(transition.moveProgress, 0.0f, 1.0f);
            if (EqualsIgnoreCase(
                    g_state.slideOutFromModeId, "EyeZoom") &&
                g_state.configSnapshot->eyezoom.slideMirrorsIn) {
                const int targetViewportX = (std::max)(
                    0,
                    (static_cast<int>(
                         context.swapchain->extent.width) -
                     g_state.configSnapshot->eyezoom.windowWidth) /
                        2);
                const int animatedViewportX =
                    g_eyeZoomAnimatedViewportX.load(
                        std::memory_order_acquire);
                if (targetViewportX > 0 &&
                    animatedViewportX >= 0) {
                    progress = std::clamp(
                        static_cast<float>(animatedViewportX) /
                            static_cast<float>(targetViewportX),
                        0.0f, 1.0f);
                }
            }
            DrawMirrors(
                context, mirrorSource, timestampFrame, nullptr,
                &g_state.slideOutMirrors, progress);
            slideOutMirrorsDrawn = true;
        };
        if (mode) {
            for (const ModeSourceRef& source : mode->sources) {
                if (source.type != ModeSourceType::Mirror &&
                    source.type != ModeSourceType::MirrorGroup) {
                    drawSlideOutMirrors();
                }
                switch (source.type) {
                case ModeSourceType::Mirror:
                    for (const MirrorConfig& mirror : g_state.mirrors) {
                        if (!mirror.runtimeGrouped && mirror.name == source.id) {
                            DrawMirrors(
                                context, mirrorSource, timestampFrame, &mirror);
                        }
                    }
                    break;
                case ModeSourceType::MirrorGroup:
                    for (const MirrorConfig& mirror : g_state.mirrors) {
                        if (mirror.runtimeGrouped &&
                            mirror.runtimeGroupName == source.id) {
                            DrawMirrors(
                                context, mirrorSource, timestampFrame, &mirror);
                        }
                    }
                    break;
                case ModeSourceType::Image:
                    for (const ImageConfig& image : g_state.images) {
                        if (image.name == source.id) DrawImages(context, &image);
                    }
                    break;
                case ModeSourceType::WindowOverlay:
                    for (const WindowOverlayConfig* overlay :
                         g_state.windowOverlays) {
                        if (overlay && overlay->name == source.id) {
                            DrawWindowOverlays(context, overlay);
                        }
                    }
                    break;
                case ModeSourceType::BrowserOverlay:
                    for (const BrowserOverlayConfig* overlay :
                         g_state.browserOverlays) {
                        if (overlay && overlay->name == source.id) {
                            DrawBrowserOverlays(context, overlay);
                        }
                    }
                    break;
                }
            }
            drawSlideOutMirrors();
        } else {
            DrawMirrors(context, mirrorSource, timestampFrame);
            DrawImages(context);
            DrawBrowserOverlays(context);
            DrawWindowOverlays(context);
        }
        // Match the OpenGL OBS composition: the key-rebind status indicator is
        // a local/game-window diagnostic, not an OBS composition source.  It
        // also owns persistent fade state, so the passive OBS pass must not
        // advance it a second time.
        if (!obsPass) {
            DrawRebindIndicator(context);
        }
        if (g_state.configSnapshot) {
            const NinjabrainOverlayConfig& ninjabrain =
                g_state.configSnapshot->ninjabrainOverlay;
            const auto ninjabrainData = g_obsCompositionPass
                ? g_state.frameNinjabrainData
                : GetNinjabrainDataSnapshot();
            if (!g_obsCompositionPass) {
                g_state.frameNinjabrainData = ninjabrainData;
            }
            bool ninjabrainStale = false;
            if (ninjabrain.hideIfStale) {
                const auto lastUpdate =
                    ninjabrainData
                        ? ninjabrainData->lastUpdateTime
                        : std::chrono::steady_clock::time_point{};
                ninjabrainStale =
                    lastUpdate == std::chrono::steady_clock::time_point{} ||
                    std::chrono::steady_clock::now() - lastUpdate >=
                        std::chrono::seconds((std::max)(
                            1, ninjabrain.hideIfStaleDelaySeconds));
            }
            const bool ninjabrainVisibleForPass =
                g_obsCompositionPass ? !ninjabrain.onlyOnMyScreen
                                     : !ninjabrain.onlyOnObs;
            if (ninjabrain.enabled && ninjabrainVisibleForPass &&
                !ninjabrainStale &&
                g_ninjabrainOverlayVisible.load(std::memory_order_acquire)) {
                PROFILE_SCOPE_CAT("Vulkan Ninjabrain overlay", "Vulkan");
                NinjabrainOverlayTextures textures;
                for (size_t index = 0;
                     index < kNinjabrainBoatTextureIds.size(); ++index) {
                    const auto asset = g_state.textureAssets.find(
                        kNinjabrainBoatTextureIds[index]);
                    if (asset != g_state.textureAssets.end()) {
                        TextureFrame* frame =
                            ResolveTextureFrame(asset->second, true);
                        if (frame && frame->sampled.linearDescriptor) {
                            textures.boatIcons[index] =
                                reinterpret_cast<uintptr_t>(
                                    frame->sampled.linearDescriptor);
                        }
                    }
                }
                for (size_t index = 0;
                     index < kNinjabrainMessageTextureIds.size(); ++index) {
                    const auto asset = g_state.textureAssets.find(
                        kNinjabrainMessageTextureIds[index]);
                    if (asset != g_state.textureAssets.end()) {
                        TextureFrame* frame =
                            ResolveTextureFrame(asset->second, true);
                        if (frame && frame->sampled.linearDescriptor) {
                            textures.messageIcons[index] =
                                reinterpret_cast<uintptr_t>(
                                    frame->sampled.linearDescriptor);
                        }
                    }
                }
                RenderNinjabrainOverlay(
                    ninjabrain, GetNinjabrainFont(), g_state.modeId, true,
                    &textures, !g_obsCompositionPass,
                    ninjabrainData);
            }
        }
        DrawCursorVisuals(context);
        if (!g_obsCompositionPass) {
            DrawNativeTextureGrid(context, mirrorSource);
        }
        if (!g_obsCompositionPass && mode &&
            g_showGui.load(std::memory_order_acquire)) {
            const int gameWidth = context.sourceMetadata
                ? static_cast<int>(
                      context.sourceMetadata->extent.width)
                : static_cast<int>(
                      context.swapchain->extent.width);
            const int gameHeight = context.sourceMetadata
                ? static_cast<int>(
                      context.sourceMetadata->extent.height)
                : static_cast<int>(
                      context.swapchain->extent.height);
            ProcessNativeEditorInteractions(
                mode, gameWidth, gameHeight);
        }
        // The overlay is backed solely by ImGui draw data, which the Vulkan backend
        // records into Minecraft's final command buffer.  It must remain separate
        // from the legacy OpenGL selection-handle implementation.
        if (!g_obsCompositionPass) {
            RenderNativeEditorOverlays();
        }
        if (g_state.configSnapshot && !g_obsCompositionPass) {
            const bool showPerformance = g_state.configSnapshot->debug.showPerformanceOverlay;
            const bool showProfiler = g_state.configSnapshot->debug.showProfiler;
            if (showPerformance) RenderPerformanceOverlay(true);
            if (showProfiler) RenderProfilerOverlay(true, showPerformance);
        }
        if (!g_obsCompositionPass &&
            g_showGui.load(std::memory_order_acquire)) {
            RenderSettingsGUI();
            RenderInteractiveCreateBanner();
            RenderMirrorSelectionInfoPanel();
            RenderMirrorGroupSelectionInfoPanel();
            RenderWindowOverlaySelectionInfoPanel();
            RenderImageSelectionInfoPanel();
            ImGuiInputQueue_PublishCaptureState();
        }
        if (!g_obsCompositionPass) DrawStartupIndicator(context);
    }
    ImGui::Render();
    g_activeFrameContext = nullptr;
    LogGuiLifecycle(obsPass ? "obs-passive" : "game-interactive",
                    ImGui::GetCurrentContext(), !obsPass);
}

void CaptureSettingsGuiDrawData() {
    g_state.settingsGuiDrawData = {};
    g_state.settingsGuiDrawList = nullptr;
    if (!g_showGui.load(std::memory_order_acquire)) return;
    const std::string title = "Toolscreen v" + GetToolscreenVersionString() + " by jojoe77777";
    ImGuiWindow* window = ImGui::FindWindowByName(title.c_str());
    const ImDrawData* full = ImGui::GetDrawData();
    if (!window || !full || !full->Valid || !window->DrawList) return;

    // Reuse the completed settings UI in OBS. It is built once by the
    // interactive context; OBS never re-runs settings logic or input.
    // Popups/modals/tooltips use separate root draw lists, so include those
    // while deliberately excluding profiler and other ordinary/debug windows.
    g_state.settingsGuiDrawList = window->DrawList;
    g_state.settingsGuiDrawData = *full;
    g_state.settingsGuiDrawData.CmdLists.clear();
    g_state.settingsGuiDrawData.TotalIdxCount = 0;
    g_state.settingsGuiDrawData.TotalVtxCount = 0;
    for (ImDrawList* list : full->CmdLists) {
        if (!list) continue;
        bool include = list == window->DrawList;
        if (!include && list->_OwnerName) {
            if (ImGuiWindow* owner =
                    ImGui::FindWindowByName(list->_OwnerName)) {
                include =
                    owner->Active &&
                    (owner->Flags &
                     (ImGuiWindowFlags_Popup |
                      ImGuiWindowFlags_Modal |
                      ImGuiWindowFlags_Tooltip)) != 0;
            }
        }
        if (!include) continue;
        g_state.settingsGuiDrawData.CmdLists.push_back(list);
        g_state.settingsGuiDrawData.TotalIdxCount += list->IdxBuffer.Size;
        g_state.settingsGuiDrawData.TotalVtxCount += list->VtxBuffer.Size;
    }
    g_state.settingsGuiDrawData.CmdListsCount =
        g_state.settingsGuiDrawData.CmdLists.Size;
}

void RestoreMirrorSource(const VulkanRenderer::FinalBlitContext& context, VkImageLayout sampleLayout,
                         SampledImage* sampled) {
    if (!sampled || sampled->image != context.sourceImage) return;
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

void RecordScreenshotCopy(
    const VulkanRenderer::FinalBlitContext& context, TimestampFrame* frame) {
    if (!g_screenshotRequested.exchange(false, std::memory_order_acq_rel))
        return;
    for (const TimestampFrame& pending : g_state.timestamps) {
        if (pending.pending && pending.screenshotPending) {
            // The clipboard path has one persistent mapped buffer. Preserve a
            // later click until its previous GPU write has been harvested.
            g_screenshotRequested.store(true, std::memory_order_release);
            return;
        }
    }
    if (!frame || !g_state.cmdCopyImageToBuffer ||
        !context.destinationMetadata ||
        (context.destinationMetadata->usage &
         VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0 ||
        !IsScreenshotFormatSupported(context.destinationMetadata->format)) {
        Log("[VULKAN][SCREENSHOT] Capture unavailable for this swapchain "
            "format/usage; request was not copied.");
        return;
    }
    const uint32_t width = context.swapchain->extent.width;
    const uint32_t height = context.swapchain->extent.height;
    if (width == 0 || height == 0 ||
        static_cast<uint64_t>(width) * static_cast<uint64_t>(height) >
            (512ull * 1024ull * 1024ull) / 4ull) {
        Log("[VULKAN][SCREENSHOT] Capture dimensions are invalid or exceed the "
            "512 MiB safety limit.");
        return;
    }
    const VkDeviceSize bytes =
        static_cast<VkDeviceSize>(width) * height * 4u;
    if (!EnsureScreenshotReadbackResources(bytes)) {
        Log("[VULKAN][SCREENSHOT] Host-coherent readback resources are not "
            "available yet; request was skipped.");
        return;
    }

    VkImageMemoryBarrier toCopy = MakeBarrier(
        context.destinationImage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    g_state.dispatch.cmdPipelineBarrier(
        context.commandBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &toCopy);

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {width, height, 1};
    g_state.cmdCopyImageToBuffer(
        context.commandBuffer, context.destinationImage,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        g_state.screenshotReadbackBuffer, 1, &copy);

    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = g_state.screenshotReadbackBuffer;
    toHost.size = bytes;
    g_state.dispatch.cmdPipelineBarrier(
        context.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0, nullptr);

    VkImageMemoryBarrier backToColor = MakeBarrier(
        context.destinationImage, VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    g_state.dispatch.cmdPipelineBarrier(
        context.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
        nullptr, 1, &backToColor);

    frame->screenshotPending = true;
    frame->screenshotWidth = static_cast<int>(width);
    frame->screenshotHeight = static_cast<int>(height);
    frame->screenshotFormat = context.destinationMetadata->format;
    Log("[VULKAN][SCREENSHOT] Copy recorded in the existing Minecraft frame "
        "command buffer; clipboard publication will follow GPU completion.");
}

ObsCompositionSlot* BeginObsComposition(
    const VulkanRenderer::FinalBlitContext& context,
    PFN_vkCmdBlitImage originalBlit,
    bool virtualCameraCaptureRequested, TimestampFrame* timestamp,
    uint32_t& selectedIndex) {
    selectedIndex = UINT32_MAX;
    // The composition image remains referenced by this game command buffer
    // until its existing timestamp query becomes available. Without a
    // timestamp slot there is no non-blocking proof of completion, so retain
    // the last published frame instead of risking ring reuse.
    if (!timestamp) return nullptr;
    const bool obsCaptureReady =
        GetModuleHandleW(L"graphics-hook64.dll") &&
        GetModuleHandleW(L"Toolscreen.dll") &&
        g_obsExportReady.load(std::memory_order_acquire);
    if ((!obsCaptureReady && !virtualCameraCaptureRequested) ||
        !context.destinationMetadata ||
        !context.dispatch->cmdClearColorImage ||
        (context.destinationMetadata->usage &
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        return nullptr;
    }
    if (!virtualCameraCaptureRequested && !ShouldUpdateObsTextureNow()) {
        ++g_state.obsCompositionThrottleCount;
        if (ShouldLogObsCapture(g_lastObsCompositionLogTick)) {
            Log("[VULKAN][OBS][COMPOSE] OBS FPS throttle reused the published "
                "composition; throttledFrames=" +
                std::to_string(g_state.obsCompositionThrottleCount) +
                ", targetFps=" + std::to_string(GetObsTargetFramerate()) +
                ".");
        }
        return nullptr;
    }

    std::lock_guard lock(g_obsCompositionMutex);
    for (uint32_t attempt = 0; attempt < kObsCompositionSlotCount; ++attempt) {
        const uint32_t index =
            (g_state.nextObsCompositionSlot + attempt) %
            kObsCompositionSlotCount;
        ObsCompositionSlot& slot = g_state.obsCompositionSlots[index];
        if (slot.recording || slot.gameWriterPending ||
            slot.obsReaderCount != 0 ||
            slot.virtualCameraReaderCount != 0)
            continue;

        ImageMetadata metadata = *context.destinationMetadata;
        metadata.extent = {
            context.swapchain->extent.width,
            context.swapchain->extent.height, 1};
        metadata.samples = VK_SAMPLE_COUNT_1_BIT;
        if (!MirrorCopyMatches(slot.image, metadata)) {
            if (slot.image.sampled.image) {
                RetireMirrorCopyImage(std::move(slot.image));
            }
            slot = {};
            if (!CreateObsCompositionImage(metadata, slot.image)) {
                Log("[VULKAN][OBS][COMPOSE] Failed to allocate native "
                    "composition image for slot=" +
                    std::to_string(index) + " extent=" +
                    std::to_string(metadata.extent.width) + "x" +
                    std::to_string(metadata.extent.height) + " format=" +
                    std::to_string(static_cast<int>(metadata.format)) +
                    "; capture remains pass-through.");
                continue;
            }
        }

        const VkImageLayout oldLayout =
            slot.image.initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                   : VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageMemoryBarrier toTransfer = MakeBarrier(
            slot.image.sampled.image,
            slot.image.initialized ? VK_ACCESS_TRANSFER_READ_BIT : 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, oldLayout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        context.dispatch->cmdPipelineBarrier(
            context.commandBuffer,
            slot.image.initialized ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                   : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &toTransfer);
        VkClearColorValue clear{};
        clear.float32[3] = 1.0f;
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        context.dispatch->cmdClearColorImage(
            context.commandBuffer, slot.image.sampled.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        const VkImageBlit obsAdjusted =
            BuildAdjustedBlit(context, true);
        originalBlit(
            context.commandBuffer, context.sourceImage, context.sourceLayout,
            slot.image.sampled.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
            &obsAdjusted, context.filter);
        VkImageMemoryBarrier toColor = MakeBarrier(
            slot.image.sampled.image, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        context.dispatch->cmdPipelineBarrier(
            context.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
            nullptr, 1, &toColor);

        slot.recording = true;
        slot.gameWriterPending = true;
        slot.published = false;
        slot.recordingCommandBuffer = context.commandBuffer;
        slot.serial = g_state.nextObsCompositionSerial++;
        slot.frameSerial =
            g_obsCompositionFrameSerial.fetch_add(
                1, std::memory_order_acq_rel) +
            1;
        g_state.nextObsCompositionSlot =
            (index + 1) % kObsCompositionSlotCount;
        g_state.lastObsCompositionUpdate = std::chrono::steady_clock::now();
        selectedIndex = index;
        timestamp->compositionWritePending = true;
        timestamp->compositionWriteSlot = index;
        timestamp->compositionWriteSerial = slot.serial;
        return &slot;
    }

    g_obsRedirectFallbackCount.fetch_add(1, std::memory_order_acq_rel);
    if (ShouldLogObsCapture(g_lastObsCompositionLogTick)) {
        Log("[VULKAN][OBS][COMPOSE] No safe ring slot is writable; all " +
            std::to_string(kObsCompositionSlotCount) +
            " slots are recording or held by OBS. Existing capture remains "
            "pass-through/published without waiting.");
    }
    return nullptr;
}

bool RecordVirtualCameraFrame(
    const VulkanRenderer::FinalBlitContext& context,
    ObsCompositionSlot& composition, uint32_t compositionSlotIndex,
    TimestampFrame* timestamp) {
    if (!timestamp || !g_state.virtualCameraPipeline ||
        !g_state.virtualCameraPipelineLayout ||
        !g_state.updateDescriptorSets || !g_state.cmdBindPipeline ||
        !g_state.cmdBindDescriptorSets || !g_state.cmdPushConstants ||
        !g_state.cmdDispatch || compositionSlotIndex >=
            kObsCompositionSlotCount) {
        return false;
    }

    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    static const LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value;
    }();
    const uint64_t timestamp100ns =
        frequency.QuadPart > 0
            ? static_cast<uint64_t>(
                  counter.QuadPart * 10000000ull / frequency.QuadPart)
            : 0;
    VirtualCameraGpuFrame frame{};
    if (!AcquireVirtualCameraGpuFrame(timestamp100ns, frame)) {
        ++g_state.virtualCameraDroppedCount;
        return false;
    }
    if (frame.slot >= g_state.virtualCameraSlots.size()) {
        AbandonVirtualCameraGpuFrame(frame);
        ++g_state.virtualCameraDroppedCount;
        return false;
    }
    VulkanVirtualCameraSlot& destination =
        g_state.virtualCameraSlots[frame.slot];
    if (!EnsureVirtualCameraSlot(frame, destination) ||
        !destination.descriptorSet) {
        AbandonVirtualCameraGpuFrame(frame);
        ++g_state.virtualCameraDroppedCount;
        Log("[VULKAN][VIRTUALCAM] Failed to import a page-aligned shared-memory "
            "slot; no CPU readback fallback was attempted.");
        return false;
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = g_state.linearSampler;
    imageInfo.imageView = composition.image.sampled.view;
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = destination.buffer;
    bufferInfo.offset = 0;
    bufferInfo.range = frame.capacityBytes;
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = destination.descriptorSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &imageInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = destination.descriptorSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &bufferInfo;
    g_state.updateDescriptorSets(
        g_state.device, static_cast<uint32_t>(writes.size()), writes.data(),
        0, nullptr);

    VkImageMemoryBarrier toCompute = MakeBarrier(
        composition.image.sampled.image, VK_ACCESS_TRANSFER_READ_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkBufferMemoryBarrier bufferToCompute{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bufferToCompute.srcAccessMask = VK_ACCESS_HOST_READ_BIT;
    bufferToCompute.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bufferToCompute.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferToCompute.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferToCompute.buffer = destination.buffer;
    bufferToCompute.size = frame.capacityBytes;
    g_state.dispatch.cmdPipelineBarrier(
        context.commandBuffer,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
        &bufferToCompute, 1, &toCompute);

    g_state.cmdBindPipeline(
        context.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_state.virtualCameraPipeline);
    g_state.cmdBindDescriptorSets(
        context.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
        g_state.virtualCameraPipelineLayout, 0, 1,
        &destination.descriptorSet, 0, nullptr);
    const std::array<uint32_t, 6> push{
        frame.width, frame.height,
        context.swapchain->extent.width,
        context.swapchain->extent.height,
        (frame.width >= 1280 || frame.height > 576) ? 1u : 0u, 0u};
    g_state.cmdPushConstants(
        context.commandBuffer, g_state.virtualCameraPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push.data());
    const uint64_t byteCount =
        static_cast<uint64_t>(frame.width) * frame.height * 3ull / 2ull;
    const uint64_t wordCount = (byteCount + 3ull) / 4ull;
    const uint32_t workgroups =
        static_cast<uint32_t>((wordCount + 255ull) / 256ull);
    g_state.cmdDispatch(context.commandBuffer, workgroups, 1, 1);

    VkBufferMemoryBarrier bufferToHost{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bufferToHost.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bufferToHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bufferToHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferToHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bufferToHost.buffer = destination.buffer;
    bufferToHost.size = frame.capacityBytes;
    VkImageMemoryBarrier backToCapture = MakeBarrier(
        composition.image.sampled.image, VK_ACCESS_SHADER_READ_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    g_state.dispatch.cmdPipelineBarrier(
        context.commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
        nullptr, 1, &bufferToHost, 1, &backToCapture);

    timestamp->virtualCameraPending = true;
    timestamp->virtualCameraFrame = frame;
    timestamp->virtualCameraCompositionSlot = compositionSlotIndex;
    timestamp->virtualCameraCompositionSerial = composition.serial;
    {
        std::lock_guard lock(g_obsCompositionMutex);
        if (composition.serial !=
            timestamp->virtualCameraCompositionSerial) {
            timestamp->virtualCameraPending = false;
            timestamp->virtualCameraFrame = {};
            timestamp->virtualCameraCompositionSlot = UINT32_MAX;
            timestamp->virtualCameraCompositionSerial = 0;
            AbandonVirtualCameraGpuFrame(frame);
            ++g_state.virtualCameraDroppedCount;
            return false;
        }
        ++composition.virtualCameraReaderCount;
    }
    ++g_state.virtualCameraRecordedCount;
    if (ShouldLogObsCapture(g_lastObsCompositionLogTick)) {
        Log("[VULKAN][VIRTUALCAM] Direct NV12 compute recorded in the existing "
            "Minecraft command buffer: size=" +
            std::to_string(frame.width) + "x" +
            std::to_string(frame.height) + ", sharedSlot=" +
            std::to_string(frame.slot) + ", compositionSlot=" +
            std::to_string(compositionSlotIndex) + ", recorded=" +
            std::to_string(g_state.virtualCameraRecordedCount) +
            ", published=" +
            std::to_string(g_state.virtualCameraPublishedCount) +
            ", dropped=" +
            std::to_string(g_state.virtualCameraDroppedCount) + ".");
    }
    return true;
}

bool RecordDrawData(
    const VulkanRenderer::FinalBlitContext& context, VkImageView view,
    ImDrawData* drawData = nullptr) {
    if (!view) return false;
    ImGui::SetCurrentContext(g_state.imguiContext);
    if (!drawData) drawData = ImGui::GetDrawData();
    if (!drawData || !drawData->Valid || drawData->CmdListsCount == 0) return true;
    VkRenderingAttachmentInfo color{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea.extent = context.swapchain->extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    if (context.dispatch->cmdBeginRendering &&
        context.dispatch->cmdEndRendering) {
        context.dispatch->cmdBeginRendering(
            context.commandBuffer, &rendering);
        g_activeImGuiCommandBuffer = context.commandBuffer;
        ImGui_ImplVulkan_RenderDrawData(
            drawData, context.commandBuffer);
        g_activeImGuiCommandBuffer = VK_NULL_HANDLE;
        context.dispatch->cmdEndRendering(context.commandBuffer);
        return true;
    }
    if (context.dispatch->cmdBeginRenderingKHR &&
        context.dispatch->cmdEndRenderingKHR) {
        context.dispatch->cmdBeginRenderingKHR(
            context.commandBuffer, &rendering);
        g_activeImGuiCommandBuffer = context.commandBuffer;
        ImGui_ImplVulkan_RenderDrawData(
            drawData, context.commandBuffer);
        g_activeImGuiCommandBuffer = VK_NULL_HANDLE;
        context.dispatch->cmdEndRenderingKHR(context.commandBuffer);
        return true;
    }
    return false;
}

bool FinishObsComposition(
    const VulkanRenderer::FinalBlitContext& context,
    ObsCompositionSlot& slot, uint32_t slotIndex,
    SampledImage* mirrorSource, SampledImage* eyeZoomSource) {
    VulkanRenderer::FinalBlitContext obsContext = context;
    obsContext.destinationImage = slot.image.sampled.image;
    obsContext.destinationLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ImageMetadata metadata = *context.destinationMetadata;
    metadata.extent = {
        context.swapchain->extent.width, context.swapchain->extent.height, 1};
    metadata.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    metadata.samples = VK_SAMPLE_COUNT_1_BIT;
    obsContext.destinationMetadata = &metadata;

    g_obsCompositionPass = true;
    GenerateImGui(obsContext, mirrorSource, eyeZoomSource, nullptr);
    // GenerateImGui leaves the passive OBS context current. Preserve its
    // completed, visibility-filtered draw data before RecordDrawData switches
    // to the interactive context that owns the Vulkan renderer backend.
    ImDrawData* obsDrawData = ImGui::GetDrawData();
    const bool rendered =
        RecordDrawData(obsContext, slot.image.sampled.view, obsDrawData);
    const int settingsGuiListCount =
        g_state.settingsGuiDrawData.Valid
            ? g_state.settingsGuiDrawData.CmdListsCount
            : 0;
    const int obsCompositionListCount =
        obsDrawData && obsDrawData->Valid
            ? obsDrawData->CmdListsCount
            : 0;
    const int obsCompositionIndexCount =
        obsDrawData && obsDrawData->Valid
            ? obsDrawData->TotalIdxCount
            : 0;
    if (rendered && settingsGuiListCount > 0) {
        RecordDrawData(obsContext, slot.image.sampled.view,
                       &g_state.settingsGuiDrawData);
    }
    g_obsCompositionPass = false;

    VkImageMemoryBarrier toCapture = MakeBarrier(
        slot.image.sampled.image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    context.dispatch->cmdPipelineBarrier(
        context.commandBuffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
        &toCapture);
    slot.image.initialized = true;
    if (!rendered) {
        std::lock_guard lock(g_obsCompositionMutex);
        slot.recording = false;
        slot.recordingCommandBuffer = VK_NULL_HANDLE;
        Log("[VULKAN][OBS][COMPOSE] Dynamic rendering was unavailable for the "
            "OBS composition; slot was not published and capture remains "
            "pass-through.");
        return false;
    }
    if (rendered && ShouldLogObsCapture(g_lastObsCompositionLogTick)) {
        Log("[VULKAN][OBS][COMPOSE] Native OBS-only composition command buffer "
            "recorded before capture: commandBuffer=" +
            std::to_string(reinterpret_cast<uint64_t>(
                context.commandBuffer)) +
            ", compositionImage=" +
            std::to_string(reinterpret_cast<uint64_t>(
                slot.image.sampled.image)) +
            ", extent=" +
            std::to_string(context.swapchain->extent.width) + "x" +
            std::to_string(context.swapchain->extent.height) + ", format=" +
            std::to_string(static_cast<int>(context.swapchain->format)) +
            ", finalLayout=TRANSFER_SRC_OPTIMAL, frame=" +
            std::to_string(slot.frameSerial) + ", ringSlot=" +
            std::to_string(slotIndex) + ", guiOpen=" +
            std::to_string(
                g_showGui.load(std::memory_order_acquire)) +
            ", reusedSettingsDrawLists=" +
            std::to_string(settingsGuiListCount) +
            ", passiveDrawLists=" +
            std::to_string(obsCompositionListCount) +
            ", passiveIndices=" +
            std::to_string(obsCompositionIndexCount) +
            ", passiveContext=" +
            std::to_string(reinterpret_cast<uintptr_t>(
                g_state.obsImGuiContext)) +
            ", backendContext=" +
            std::to_string(reinterpret_cast<uintptr_t>(
                g_state.imguiContext)) +
            ". Native Vulkan only; no GL object/path was used.");
    }
    return true;
}

} // namespace

namespace VulkanRenderer {

bool RecordAfterFinalBlit(const FinalBlitContext& context, PFN_vkCmdBlitImage originalBlit) {
    std::lock_guard<std::recursive_mutex> lifecycleLock(g_rendererLifecycleMutex);
    if (!originalBlit || !context.dispatch || !context.swapchain || !context.destinationMetadata) return false;
    if (!g_loggedRendererEntry.exchange(true, std::memory_order_acq_rel)) {
        Log("[VULKAN] Entering native final-blit renderer; current backend is " +
            std::string(GetRenderBackendName(GetRenderBackend())) + ".");
    }
    const RenderBackend currentBackend = GetRenderBackend();
    if (currentBackend != RenderBackend::Unknown && currentBackend != RenderBackend::Vulkan) {
        if (!g_loggedBackendConflict.exchange(true, std::memory_order_acq_rel)) {
            Log("[VULKAN] Native final blit was rejected because the renderer backend is already " +
                std::string(GetRenderBackendName(GetRenderBackend())) + ".");
        }
        return false;
    }
    if (g_deviceBeingDestroyed.load(std::memory_order_acquire)) return false;
    if (!g_state.initialized && !InitializeRenderer(context)) {
        // Still emit Minecraft's adjusted blit even if optional overlay resources
        // are not ready yet.
        VkImageBlit adjusted = BuildAdjustedBlit(context);
        originalBlit(context.commandBuffer, context.sourceImage, context.sourceLayout, context.destinationImage,
                     context.destinationLayout, 1, &adjusted, context.filter);
        return true;
    }
    // Do not make an early, incomplete Vulkan bootstrap permanently win the
    // backend race.  The renderer is now initialized against the exact
    // swapchain HWND, so this is the first authoritative native frame.
    if (!TryLatchRenderBackend(RenderBackend::Vulkan)) {
        return false;
    }
    if (g_state.device != context.device || g_state.format != context.swapchain->format) {
        g_state.rebuildDevice = context.device;
        g_state.rebuildFormat = context.swapchain->format;
        g_state.rebuildPending = true;
    }
    if (g_state.rebuildPending) {
        HarvestTimestamps();
        const bool isRequestedGeneration =
            context.device == g_state.rebuildDevice &&
            context.swapchain->format == g_state.rebuildFormat;
        if (!isRequestedGeneration || HasPendingGpuWork()) {
            // Keep Minecraft presenting while old overlay resources drain.
            // No new command buffer references them once a rebuild is pending.
            VkImageBlit adjusted = BuildAdjustedBlit(context);
            originalBlit(
                context.commandBuffer, context.sourceImage, context.sourceLayout,
                context.destinationImage, context.destinationLayout, 1, &adjusted,
                context.filter);
            return true;
        }

        const VkDevice requestedDevice = g_state.rebuildDevice;
        const VkFormat requestedFormat = g_state.rebuildFormat;
        Shutdown();
        if (context.device != requestedDevice ||
            context.swapchain->format != requestedFormat ||
            !InitializeRenderer(context)) {
            VkImageBlit adjusted = BuildAdjustedBlit(context);
            originalBlit(
                context.commandBuffer, context.sourceImage, context.sourceLayout,
                context.destinationImage, context.destinationLayout, 1, &adjusted,
                context.filter);
            return true;
        }
        LogCategory(
            "init",
            "[VULKAN] Rebuilt native renderer resources for the active device and swapchain format.");
    }

    if (!g_logicThreadRunning.load(std::memory_order_acquire) && g_configLoaded.load(std::memory_order_acquire)) {
        StartLogicThread();
    }
    if (!g_configLoaded.load(std::memory_order_acquire) &&
        !g_configLoadFailed.load(std::memory_order_acquire)) {
        return false;
    }

    g_state.dispatch = *context.dispatch;
    if (g_state.hwnd != context.swapchain->hwnd) {
        HWND hwnd = context.swapchain->hwnd;
        if (IsTrackedMinecraftWindow(hwnd) && hwnd != g_state.hwnd) {
            g_state.hwnd = hwnd;
            g_minecraftHwnd.store(hwnd, std::memory_order_release);
            SubclassGameWindow(hwnd);
        }
    }
    const auto subclassCheckNow = std::chrono::steady_clock::now();
    if (g_state.hwnd &&
        (g_state.lastSubclassCheck ==
             std::chrono::steady_clock::time_point{} ||
         subclassCheckNow - g_state.lastSubclassCheck >=
             std::chrono::seconds(1))) {
        SubclassGameWindow(g_state.hwnd);
        g_state.lastSubclassCheck = subclassCheckNow;
    }

    const Config* cfg = nullptr;
    auto cfgSnapshot = GetConfigSnapshot();
    if (cfgSnapshot) cfg = cfgSnapshot.get();
    const bool profilerEnabled = cfg && cfg->debug.showProfiler;
    Profiler::GetInstance().SetEnabled(profilerEnabled);
    if (profilerEnabled) Profiler::GetInstance().MarkAsRenderThread();
    SyncVirtualCameraRuntimeState(
        cfg && cfg->debug.virtualCameraEnabled);
    const bool virtualCameraCaptureRequested =
        IsVirtualCameraActive() && ShouldCaptureVirtualCameraFrame();

    // OpenGL advances this from its SwapBuffers hook. Vulkan does not execute
    // that path, so advance the shared transition state from the ordinary
    // Vulkan presentation pass instead.
    if (IsModeTransitionActive()) {
        UpdateModeTransition();
    }

    RefreshModeCache(
        static_cast<int>(context.swapchain->extent.width),
        static_cast<int>(context.swapchain->extent.height),
        context.sourceMetadata
            ? static_cast<int>(context.sourceMetadata->extent.width)
            : 0,
        context.sourceMetadata
            ? static_cast<int>(context.sourceMetadata->extent.height)
            : 0);
    {
        const ModeViewportInfo presented =
            ResolveSubmittedViewport(context);
        const int gameWidth = context.sourceMetadata
            ? static_cast<int>(
                  context.sourceMetadata->extent.width)
            : presented.width;
        const int gameHeight = context.sourceMetadata
            ? static_cast<int>(
                  context.sourceMetadata->extent.height)
            : presented.height;
        if (context.sourceMetadata) {
            // Mouse translation consumes the latest logical game-source size.
            // The OpenGL backend publishes this from glViewport; Vulkan's
            // authoritative equivalent is the final blit's source extent.
            PublishLatestGameViewportSize(gameWidth, gameHeight);
        }
        PublishNativeFrameGeometry(
            gameWidth, gameHeight, presented.x, presented.y,
            presented.width, presented.height);
    }
    TimestampFrame* timestamp = BeginTimestamps(context.commandBuffer);
    VkImageBlit adjusted = BuildAdjustedBlit(context);
    originalBlit(context.commandBuffer, context.sourceImage, context.sourceLayout, context.destinationImage,
                 context.destinationLayout, 1, &adjusted, context.filter);
    uint32_t obsCompositionSlotIndex = UINT32_MAX;
    ObsCompositionSlot* obsComposition = BeginObsComposition(
        context, originalBlit, virtualCameraCaptureRequested,
        timestamp, obsCompositionSlotIndex);
    ObserveNativeObsCapture();
    WriteTimestamp(context.commandBuffer, timestamp, 1, VK_PIPELINE_STAGE_TRANSFER_BIT);

    if ((context.destinationMetadata->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        for (uint32_t query = 2; query < kQueriesPerFrame; ++query) {
            WriteTimestamp(
                context.commandBuffer, timestamp, query,
                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
        return true;
    }

    PollGuiHotkeyFallback();

    VkImageLayout mirrorSampleLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    SampledImage* mirrorSource = nullptr;
    PrepareMirrorSource(context, mirrorSampleLayout, mirrorSource);
    PrepareMirrorSnapshots(
        context, mirrorSource, mirrorSampleLayout, timestamp);
    SampledImage* eyeZoomSource = PrepareEyeZoomSource(
        context, mirrorSource, mirrorSampleLayout, timestamp);
    WriteTimestamp(
        context.commandBuffer, timestamp, 2, VK_PIPELINE_STAGE_TRANSFER_BIT);
    RefreshFontResourcesIfNeeded();
    RecordFontUpload(context.commandBuffer);
    {
        PROFILE_SCOPE_CAT("Vulkan image preparation", "Vulkan");
        if (g_pendingImageLoad.exchange(false, std::memory_order_acq_rel)) {
            LoadAllImages();
            g_allImagesLoaded.store(true, std::memory_order_release);
        }
        ProcessPendingTextureAssets();
        RecordTextureUploads(context.commandBuffer);
    }
    WriteTimestamp(
        context.commandBuffer, timestamp, 3, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageMemoryBarrier toColor = MakeBarrier(
        context.destinationImage, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    context.dispatch->cmdPipelineBarrier(
        context.commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toColor);

    SampledImage* destinationView = GetImageView(context.destinationImage, *context.destinationMetadata);
    if (!destinationView) {
        if (obsComposition) {
            VkImageMemoryBarrier compositionToCapture = MakeBarrier(
                obsComposition->image.sampled.image,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            context.dispatch->cmdPipelineBarrier(
                context.commandBuffer,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                1, &compositionToCapture);
            std::lock_guard lock(g_obsCompositionMutex);
            obsComposition->image.initialized = true;
            obsComposition->recording = false;
            obsComposition->recordingCommandBuffer = VK_NULL_HANDLE;
        }
        VkImageMemoryBarrier destinationToTransfer = MakeBarrier(
            context.destinationImage,
            VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        context.dispatch->cmdPipelineBarrier(
            context.commandBuffer,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
            &destinationToTransfer);
        RestoreMirrorSource(context, mirrorSampleLayout, mirrorSource);
        WriteTimestamp(
            context.commandBuffer, timestamp, 4,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        WriteTimestamp(
            context.commandBuffer, timestamp, 5,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        return true;
    }

    {
        PROFILE_SCOPE_CAT("Vulkan resource upload", "Vulkan");
        g_activeResourceTimestampFrame = timestamp;
        GenerateImGui(context, mirrorSource, eyeZoomSource, timestamp);
        CaptureSettingsGuiDrawData();
    }
    WriteTimestamp(
        context.commandBuffer, timestamp, 4,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (timestamp && timestamp->mirrorQueryCount > 0 &&
        g_state.mirrorQueryPool && g_state.dispatch.cmdResetQueryPool) {
        g_state.dispatch.cmdResetQueryPool(
            context.commandBuffer, g_state.mirrorQueryPool,
            timestamp->mirrorFirstQuery, timestamp->mirrorQueryCount);
    }

    {
        PROFILE_SCOPE_CAT("Vulkan overlay recording", "Vulkan");
        const bool recordedNativeOverlay =
            RecordDrawData(context, destinationView->view);
        if (recordedNativeOverlay &&
            !g_loggedFirstNativeOverlayFrame.exchange(true, std::memory_order_acq_rel)) {
            Log("[VULKAN] First native dynamic-rendering overlay frame recorded.");
        }
    }
    if (obsComposition) {
        const bool compositionComplete = FinishObsComposition(
            context, *obsComposition, obsCompositionSlotIndex, mirrorSource,
            eyeZoomSource);
        if (compositionComplete && virtualCameraCaptureRequested) {
            (void)RecordVirtualCameraFrame(
                context, *obsComposition, obsCompositionSlotIndex, timestamp);
        }
    }
    g_activeResourceTimestampFrame = nullptr;
    RecordScreenshotCopy(context, timestamp);
    VkImageMemoryBarrier toTransfer = MakeBarrier(
        context.destinationImage, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    context.dispatch->cmdPipelineBarrier(
        context.commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &toTransfer);
    RestoreMirrorSource(context, mirrorSampleLayout, mirrorSource);
    RecordColorPickerSample(context, timestamp);
    WriteTimestamp(
        context.commandBuffer, timestamp, 5,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    Profiler::GetInstance().EndFrame();
    // Keep only handle values for diagnostics.  These are atomics because the
    // queue submit/present calls may arrive from a different Minecraft thread.
    g_lastNativeOverlayCommandBuffer.store(reinterpret_cast<uint64_t>(context.commandBuffer), std::memory_order_release);
    g_lastNativeOverlayDestinationImage.store(reinterpret_cast<uint64_t>(context.destinationImage), std::memory_order_release);
    g_lastNativeOverlaySerial.fetch_add(1, std::memory_order_acq_rel);
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

bool GetBundledGuiTexture(int resourceId, uintptr_t& textureId) {
    textureId = 0;
    if (!g_state.initialized) return false;
    const char* id = nullptr;
    switch (resourceId) {
    case IDR_LANGUAGE_PNG: id = kGuiLanguageTextureId; break;
    case IDR_DISCORD_PNG: id = kGuiDiscordTextureId; break;
    case IDR_EDITOR_PNG: id = kGuiEditorTextureId; break;
    default: return false;
    }
    auto asset = g_state.textureAssets.find(id);
    if (asset == g_state.textureAssets.end()) return false;
    TextureFrame* frame = ResolveTextureFrame(asset->second, false);
    if (!frame || !frame->sampled.descriptor) return false;
    textureId = reinterpret_cast<uintptr_t>(frame->sampled.descriptor);
    return true;
}

bool GetGuiRgbaTexture(
    const std::string& key, const unsigned char* pixels, int width, int height,
    uint64_t generation, uintptr_t& textureId) {
    textureId = 0;
    if (!g_state.initialized || g_obsCompositionPass ||
        !g_activeFrameContext || !pixels || width <= 0 || height <= 0 ||
        generation == 0) {
        return false;
    }
    TextureFrame* frame = PrepareStreamingTexture(
        "gui:" + key, g_activeFrameContext->destinationImage, pixels, width,
        height, generation, g_activeFrameContext->commandBuffer, true);
    if (!frame || !frame->sampled.linearDescriptor) return false;
    textureId = reinterpret_cast<uintptr_t>(
        frame->sampled.linearDescriptor);
    return true;
}

void OnQueueSubmit(VkDevice, VkQueue queue, uint32_t commandBufferCount, const VkCommandBuffer* commandBuffers, VkFence fence) {
    // Timestamp availability is polled without VK_QUERY_RESULT_WAIT_BIT on a
    // later frame. No fence wait or second overlay submission is introduced.
    if (!commandBuffers) return;
    {
        std::lock_guard lock(g_obsCompositionMutex);
        for (uint32_t index = 0; index < kObsCompositionSlotCount; ++index) {
            ObsCompositionSlot& slot = g_state.obsCompositionSlots[index];
            if (!slot.recording) continue;
            for (uint32_t bufferIndex = 0;
                 bufferIndex < commandBufferCount; ++bufferIndex) {
                if (commandBuffers[bufferIndex] !=
                    slot.recordingCommandBuffer) {
                    continue;
                }
                if (g_state.publishedObsCompositionSlot <
                        kObsCompositionSlotCount &&
                    g_state.publishedObsCompositionSlot != index) {
                    g_state
                        .obsCompositionSlots
                            [g_state.publishedObsCompositionSlot]
                        .published = false;
                }
                slot.recording = false;
                slot.recordingCommandBuffer = VK_NULL_HANDLE;
                slot.published = true;
                g_state.publishedObsCompositionSlot = index;
                if (ShouldLogObsCapture(g_lastObsPublishLogTick)) {
                    Log("[VULKAN][OBS][COMPOSE] Composition command buffer "
                        "submitted before capture/present: commandBuffer=" +
                        std::to_string(reinterpret_cast<uint64_t>(
                            commandBuffers[bufferIndex])) +
                        ", queue=" +
                        std::to_string(reinterpret_cast<uint64_t>(queue)) +
                        ", gameFence=" +
                        std::to_string(reinterpret_cast<uint64_t>(fence)) +
                        ", frame=" + std::to_string(slot.frameSerial) +
                        ", ringSlot=" + std::to_string(index) + ", serial=" +
                        std::to_string(slot.serial) + ".");
                }
                break;
            }
        }
    }
    if (!GetModuleHandleA("graphics-hook64.dll")) return;
    const uint64_t overlayBuffer = g_lastNativeOverlayCommandBuffer.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < commandBufferCount; ++i) {
        if (reinterpret_cast<uint64_t>(commandBuffers[i]) != overlayBuffer) continue;
        if (ShouldLogObsCapture(g_lastObsSubmitLogTick)) {
            Log("[VULKAN][OBS][SUBMIT] Native overlay command buffer=" + std::to_string(overlayBuffer) +
                " serial=" + std::to_string(g_lastNativeOverlaySerial.load(std::memory_order_acquire)) +
                " is submitted on queue=" + std::to_string(reinterpret_cast<uint64_t>(queue)) +
                " fence=" + std::to_string(reinterpret_cast<uint64_t>(fence)) + ".");
        }
        break;
    }
}

void OnQueuePresent(VkDevice, VkQueue queue, const VkPresentInfoKHR* presentInfo,
                    const std::vector<VkImage>& presentImages) {
    if (!GetModuleHandleA("graphics-hook64.dll") || !presentInfo || !presentInfo->pSwapchains || !presentInfo->pImageIndices) return;
    if (!ShouldLogObsCapture(g_lastObsPresentLogTick)) return;

    const uint64_t overlayImage = g_lastNativeOverlayDestinationImage.load(std::memory_order_acquire);
    const uint64_t overlayBuffer = g_lastNativeOverlayCommandBuffer.load(std::memory_order_acquire);
    Log("[VULKAN][OBS][PRESENT] forwarding vkQueuePresentKHR to OBS: queue=" +
        std::to_string(reinterpret_cast<uint64_t>(queue)) + ", swapchains=" +
        std::to_string(presentInfo->swapchainCount) + ", lastOverlayCB=" +
        std::to_string(overlayBuffer) + ", lastOverlayDestinationImage=" +
        std::to_string(overlayImage) + ", overlaySerial=" +
        std::to_string(g_lastNativeOverlaySerial.load(std::memory_order_acquire)) + ".");
    for (uint32_t i = 0; i < presentInfo->swapchainCount; ++i) {
        Log("[VULKAN][OBS][PRESENT] item=" + std::to_string(i) + ", swapchain=" +
            std::to_string(reinterpret_cast<uint64_t>(presentInfo->pSwapchains[i])) + ", imageIndex=" +
            std::to_string(presentInfo->pImageIndices[i]) + ", trackedImage=" +
            std::to_string(i < presentImages.size() ? reinterpret_cast<uint64_t>(presentImages[i]) : 0) +
            ", matchesLastOverlayDestination=" +
            std::string(i < presentImages.size() && reinterpret_cast<uint64_t>(presentImages[i]) == overlayImage ? "true" : "false") +
            ", redirectSubmitted=" +
            std::to_string(g_obsRedirectSubmittedCount.load(std::memory_order_acquire)) +
            ", redirectRetired=" +
            std::to_string(g_obsRedirectRetiredCount.load(std::memory_order_acquire)) +
            ", fallback=" +
            std::to_string(g_obsRedirectFallbackCount.load(std::memory_order_acquire)) +
            ". The upper OBS hook remains untouched; the lower process-local layer "
            "redirects only its validated export copy while this real swapchain image is still presented.");
    }
}

void OnImageDestroyed(VkDevice device, VkImage image) {
    std::lock_guard<std::recursive_mutex> lifecycleLock(g_rendererLifecycleMutex);
    if (!g_state.initialized || g_state.device != device || !image) return;
    if (auto copy = g_state.mirrorCopyImages.find(image);
        copy != g_state.mirrorCopyImages.end()) {
        // Source-image destruction can be observed while Toolscreen's copy is
        // still present in a submitted final command buffer.  The copy is
        // independent of the source, so keep it alive until that frame has
        // completed instead of freeing it from inside the destroy hook.
        RetireMirrorCopyImage(std::move(copy->second));
        g_state.mirrorCopyImages.erase(copy);
    }
    auto it = g_state.imageResources.find(image);
    if (it == g_state.imageResources.end()) return;
    ImGui::SetCurrentContext(g_state.imguiContext);
    if (it->second.descriptor) ImGui_ImplVulkan_RemoveTexture(it->second.descriptor);
    if (it->second.linearDescriptor) {
        ImGui_ImplVulkan_RemoveTexture(it->second.linearDescriptor);
    }
    if (it->second.view && g_state.dispatch.destroyImageView) {
        g_state.dispatch.destroyImageView(device, it->second.view, nullptr);
    }
    g_state.imageResources.erase(it);
}

void OnSwapchainDestroyed(VkDevice device, VkSwapchainKHR, const std::vector<VkImage>& images) {
    std::lock_guard<std::recursive_mutex> lifecycleLock(g_rendererLifecycleMutex);
    if (!g_state.initialized || g_state.device != device) return;
    for (VkImage image : images) {
        OnImageDestroyed(device, image);
        for (auto& [key, texture] : g_state.streamingTextures) {
            auto slot = texture.slots.find(image);
            if (slot != texture.slots.end()) {
                DestroyStreamingTextureSlot(slot->second);
                texture.slots.erase(slot);
            }
        }
    }
}

void OnDeviceDestroyed(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lifecycleLock(g_rendererLifecycleMutex);
    if (g_state.device != device) return;
    g_deviceBeingDestroyed.store(true, std::memory_order_release);
    Shutdown();
}

void Shutdown() {
    std::lock_guard<std::recursive_mutex> lifecycleLock(g_rendererLifecycleMutex);
    g_obsExportReady.store(false, std::memory_order_release);
    const bool hasResources =
        g_state.initialized || g_state.device != VK_NULL_HANDLE ||
        g_state.imguiContext != nullptr || g_state.obsImGuiContext != nullptr ||
        g_state.mirrorSampler != VK_NULL_HANDLE || g_state.linearSampler != VK_NULL_HANDLE ||
        g_state.queryPool != VK_NULL_HANDLE || g_state.mirrorQueryPool != VK_NULL_HANDLE;
    if (!hasResources) {
        g_deviceBeingDestroyed.store(false, std::memory_order_release);
        return;
    }
    g_ready.store(false, std::memory_order_release);
    if (g_state.imguiContext) ImGui::SetCurrentContext(g_state.imguiContext);
    // Imported host memory must be released before its shared-memory lease can
    // allow the virtual-camera mapping to unmap.
    for (VulkanVirtualCameraSlot& slot : g_state.virtualCameraSlots) {
        DestroyVirtualCameraSlot(slot);
    }
    for (TimestampFrame& frame : g_state.timestamps) {
        if (frame.virtualCameraPending) {
            AbandonVirtualCameraGpuFrame(frame.virtualCameraFrame);
            frame.virtualCameraPending = false;
            frame.virtualCameraFrame = {};
        }
    }
    {
        std::lock_guard lock(g_obsCompositionMutex);
        for (ObsCompositionSlot& slot : g_state.obsCompositionSlots) {
            DestroyMirrorCopyImage(slot.image);
            slot = {};
        }
        g_state.publishedObsCompositionSlot = UINT32_MAX;
    }
    for (auto& [image, resource] : g_state.imageResources) {
        if (resource.descriptor) ImGui_ImplVulkan_RemoveTexture(resource.descriptor);
        if (resource.linearDescriptor) {
            ImGui_ImplVulkan_RemoveTexture(resource.linearDescriptor);
        }
        if (resource.view && g_state.dispatch.destroyImageView) {
            g_state.dispatch.destroyImageView(g_state.device, resource.view, nullptr);
        }
    }
    g_state.imageResources.clear();
    for (auto& [id, asset] : g_state.textureAssets) {
        DestroyTextureAsset(asset);
    }
    g_state.textureAssets.clear();
    for (RetiredTextureAsset& retired : g_state.retiredTextureAssets) {
        DestroyTextureAsset(retired.asset);
    }
    g_state.retiredTextureAssets.clear();
    for (auto& [key, texture] : g_state.streamingTextures) {
        for (auto& [frameSlot, slot] : texture.slots) {
            DestroyStreamingTextureSlot(slot);
        }
    }
    g_state.streamingTextures.clear();
    for (auto& [source, resource] : g_state.mirrorCopyImages) {
        DestroyMirrorCopyImage(resource);
    }
    g_state.mirrorCopyImages.clear();
    for (auto& [fps, snapshot] : g_state.mirrorSnapshots) {
        for (MirrorSnapshotSlot& slot : snapshot.slots) {
            DestroyMirrorCopyImage(slot.image);
        }
    }
    g_state.mirrorSnapshots.clear();
    for (RetiredMirrorSnapshot& retired : g_state.retiredMirrorSnapshots) {
        DestroyMirrorCopyImage(retired.image);
    }
    g_state.retiredMirrorSnapshots.clear();
    for (RetiredFontResource& retired :
         g_state.retiredFontResources) {
        DestroyFontResource(retired);
    }
    g_state.retiredFontResources.clear();
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
        for (const auto& [key, pipeline] : g_state.overlayPipelines) {
            if (pipeline) g_state.destroyPipeline(g_state.device, pipeline, nullptr);
        }
        if (g_state.virtualCameraPipeline) {
            g_state.destroyPipeline(
                g_state.device, g_state.virtualCameraPipeline, nullptr);
        }
    }
    g_state.mirrorPipelines.clear();
    g_state.overlayPipelines.clear();
    if (g_state.mirrorVertexShader && g_state.destroyShaderModule) {
        g_state.destroyShaderModule(g_state.device, g_state.mirrorVertexShader, nullptr);
    }
    if (g_state.mirrorFragmentShader && g_state.destroyShaderModule) {
        g_state.destroyShaderModule(g_state.device, g_state.mirrorFragmentShader, nullptr);
    }
    if (g_state.overlayFragmentShader && g_state.destroyShaderModule) {
        g_state.destroyShaderModule(
            g_state.device, g_state.overlayFragmentShader, nullptr);
    }
    if (g_state.virtualCameraComputeShader &&
        g_state.destroyShaderModule) {
        g_state.destroyShaderModule(
            g_state.device, g_state.virtualCameraComputeShader, nullptr);
    }
    if (g_state.mirrorPipelineLayout && g_state.destroyPipelineLayout) {
        g_state.destroyPipelineLayout(g_state.device, g_state.mirrorPipelineLayout, nullptr);
    }
    if (g_state.mirrorDescriptorSetLayout && g_state.destroyDescriptorSetLayout) {
        g_state.destroyDescriptorSetLayout(g_state.device, g_state.mirrorDescriptorSetLayout, nullptr);
    }
    if (g_state.virtualCameraPipelineLayout &&
        g_state.destroyPipelineLayout) {
        g_state.destroyPipelineLayout(
            g_state.device, g_state.virtualCameraPipelineLayout, nullptr);
    }
    if (g_state.virtualCameraDescriptorPool &&
        g_state.dispatch.destroyDescriptorPool) {
        g_state.dispatch.destroyDescriptorPool(
            g_state.device, g_state.virtualCameraDescriptorPool, nullptr);
    }
    if (g_state.virtualCameraDescriptorSetLayout &&
        g_state.destroyDescriptorSetLayout) {
        g_state.destroyDescriptorSetLayout(
            g_state.device, g_state.virtualCameraDescriptorSetLayout, nullptr);
    }
    if (g_state.mirrorSampler && g_state.dispatch.destroySampler) {
        g_state.dispatch.destroySampler(g_state.device, g_state.mirrorSampler, nullptr);
    }
    if (g_state.linearSampler && g_state.dispatch.destroySampler) {
        g_state.dispatch.destroySampler(g_state.device, g_state.linearSampler, nullptr);
    }
    if (g_state.imguiVulkanInitialized) ImGui_ImplVulkan_Shutdown();
    if (g_state.imguiWin32Initialized) ImGui_ImplWin32_Shutdown();
    if (g_state.obsImGuiContext) ImGui::DestroyContext(g_state.obsImGuiContext);
    g_state.obsImGuiContext = nullptr;
    if (g_state.imguiContext) ImGui::DestroyContext(g_state.imguiContext);
    if (g_state.fontUploadMapped && g_state.dispatch.unmapMemory) {
        g_state.dispatch.unmapMemory(g_state.device, g_state.fontUploadMemory);
    }
    if (g_state.pickerReadbackMapped && g_state.dispatch.unmapMemory) {
        g_state.dispatch.unmapMemory(g_state.device, g_state.pickerReadbackMemory);
    }
    if (g_state.screenshotReadbackMapped && g_state.dispatch.unmapMemory) {
        g_state.dispatch.unmapMemory(
            g_state.device, g_state.screenshotReadbackMemory);
    }
    if (g_state.pickerReadbackBuffer && g_state.dispatch.destroyBuffer) {
        g_state.dispatch.destroyBuffer(g_state.device, g_state.pickerReadbackBuffer, nullptr);
    }
    if (g_state.pickerReadbackMemory && g_state.dispatch.freeMemory) {
        g_state.dispatch.freeMemory(g_state.device, g_state.pickerReadbackMemory, nullptr);
    }
    if (g_state.screenshotReadbackBuffer &&
        g_state.dispatch.destroyBuffer) {
        g_state.dispatch.destroyBuffer(
            g_state.device, g_state.screenshotReadbackBuffer, nullptr);
    }
    if (g_state.screenshotReadbackMemory &&
        g_state.dispatch.freeMemory) {
        g_state.dispatch.freeMemory(
            g_state.device, g_state.screenshotReadbackMemory, nullptr);
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

extern "C" __declspec(dllexport) bool ToolscreenVulkanGetObsComposition(
    VkDevice device, VkImage presentedSwapchainImage, VkFormat format,
    VkExtent2D extent, ToolscreenVulkanObsComposition* result) {
    if (!result) return false;
    *result = {};
    std::lock_guard lock(g_obsCompositionMutex);
    if (!g_state.initialized || g_state.device != device ||
        reinterpret_cast<uint64_t>(presentedSwapchainImage) !=
            g_lastNativeOverlayDestinationImage.load(
                std::memory_order_acquire) ||
        g_state.publishedObsCompositionSlot >= kObsCompositionSlotCount) {
        g_obsRedirectFallbackCount.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }
    ObsCompositionSlot& slot =
        g_state.obsCompositionSlots[g_state.publishedObsCompositionSlot];
    if (!slot.published || !slot.image.initialized ||
        !slot.image.sampled.image || slot.image.format != format ||
        slot.image.extent.width != extent.width ||
        slot.image.extent.height != extent.height) {
        g_obsRedirectFallbackCount.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }
    ++slot.obsReaderCount;
    result->image = slot.image.sampled.image;
    result->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    result->format = slot.image.format;
    result->extent = extent;
    result->slot = g_state.publishedObsCompositionSlot;
    result->serial = slot.serial;
    result->frameSerial = slot.frameSerial;
    return true;
}

extern "C" __declspec(dllexport) void ToolscreenVulkanObsRedirectSubmitted(
    uint32_t slotIndex, uint64_t serial, VkFence fence) {
    std::lock_guard lock(g_obsCompositionMutex);
    if (slotIndex >= kObsCompositionSlotCount) return;
    ObsCompositionSlot& slot = g_state.obsCompositionSlots[slotIndex];
    if (slot.serial != serial || slot.obsReaderCount == 0) return;
    ++slot.obsSubmittedReaderCount;
    g_obsRedirectSubmittedCount.fetch_add(1, std::memory_order_acq_rel);
}

extern "C" __declspec(dllexport) void ToolscreenVulkanObsRedirectRetired(
    uint32_t slotIndex, uint64_t serial, VkFence fence,
    VkResult completion) {
    std::lock_guard lock(g_obsCompositionMutex);
    if (slotIndex >= kObsCompositionSlotCount) return;
    ObsCompositionSlot& slot = g_state.obsCompositionSlots[slotIndex];
    if (slot.serial != serial || slot.obsReaderCount == 0) return;
    --slot.obsReaderCount;
    if (slot.obsSubmittedReaderCount != 0)
        --slot.obsSubmittedReaderCount;
    const uint64_t retired =
        g_obsRedirectRetiredCount.fetch_add(
            1, std::memory_order_acq_rel) +
        1;
    if (ShouldLogObsCapture(g_lastObsCompositionLogTick)) {
        Log("[VULKAN][OBS][RING] OBS's own fence retired composition slot=" +
            std::to_string(slotIndex) + " serial=" +
            std::to_string(serial) + " completion=" +
            std::to_string(static_cast<int>(completion)) +
            " retiredCount=" + std::to_string(retired) +
            " remainingReaders=" + std::to_string(slot.obsReaderCount) +
            (slot.obsReaderCount == 0
                 ? "; the image is writable again without a Toolscreen wait."
                 : "; the immutable publication remains shared by later OBS reads."));
    }
}

extern "C" __declspec(dllexport) void ToolscreenVulkanObsRedirectAbandoned(
    uint32_t slotIndex, uint64_t serial, const char* reason) {
    std::lock_guard lock(g_obsCompositionMutex);
    if (slotIndex >= kObsCompositionSlotCount) return;
    ObsCompositionSlot& slot = g_state.obsCompositionSlots[slotIndex];
    if (slot.serial != serial || slot.obsReaderCount == 0) return;
    --slot.obsReaderCount;
    g_obsRedirectFallbackCount.fetch_add(1, std::memory_order_acq_rel);
    Log("[VULKAN][OBS][RING] Redirect reservation abandoned for slot=" +
        std::to_string(slotIndex) + " serial=" + std::to_string(serial) +
        ", reason=" + (reason ? std::string(reason) : "<unknown>") +
        "; stock capture remains active.");
}

extern "C" __declspec(dllexport) void ToolscreenVulkanObsCaptureAvailability(
    VkDevice device, bool available, VkImage exportImage, VkFormat format,
    VkExtent2D extent) {
    const bool ready = available;
    g_obsExportReady.store(ready, std::memory_order_release);
    Log("[VULKAN][OBS][READY] OBS export capture " +
        std::string(ready ? "ready" : "unavailable") + ": device=" +
        std::to_string(reinterpret_cast<uint64_t>(device)) +
        ", exportImage=" +
        std::to_string(reinterpret_cast<uint64_t>(exportImage)) +
        ", extent=" + std::to_string(extent.width) + "x" +
        std::to_string(extent.height) + ", format=" +
        std::to_string(static_cast<int>(format)) +
        (ready
             ? "; native composition will begin on the next real game frame."
             : "; redirect fails closed and composition recording is disabled."));
}

extern "C" __declspec(dllexport) void
ToolscreenVulkanBeforeLowerDeviceDestroy(VkDevice device) {
    Log("[VULKAN][OBS][RING] Lower-layer device teardown reached after OBS "
        "capture cleanup; destroying cached native composition resources "
        "before the driver device.");
    VulkanRenderer::OnDeviceDestroyed(device);
}

extern "C" __declspec(dllexport) void ToolscreenVulkanLayerLog(
    const char* message) {
    if (message && *message) Log(message);
}
