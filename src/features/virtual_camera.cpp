#include "virtual_camera.h"
#include "common/utils.h"
#include "render/render.h"

// Prevent Windows min/max macros from conflicting with std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>
#include <windows.h>

extern std::atomic<HWND> g_minecraftHwnd;
extern Config g_config;


std::atomic<bool> g_virtualCameraActive{ false };

static std::mutex g_vcMutex;
static std::string g_vcLastError;
static std::atomic<LONGLONG> g_vcLastCaptureTick{ 0 };
static std::atomic<int> g_vcForcedCaptureFrames{ 0 };

// Pending-resize debounce state (lock-free)
static std::atomic<uint32_t> g_vcPendingResizeWidth{ 0 };
static std::atomic<uint32_t> g_vcPendingResizeHeight{ 0 };
static std::atomic<ULONGLONG> g_vcPendingResizeRequestedMs{ 0 };

constexpr int kVirtualCameraDefaultFps = 60;
constexpr int kVirtualCameraLimitedFps = 30;
constexpr ULONGLONG kVirtualCameraResizeDebounceMs = 150;
constexpr int kVirtualCameraForcedFramesAfterReinit = 6;

#define VIDEO_NAME L"OBSVirtualCamVideo"
#define FRAME_HEADER_SIZE 32
#define ALIGN_SIZE(size, align) size = (((size) + (align - 1)) & (~(align - 1)))

enum queue_state {
    SHARED_QUEUE_STATE_INVALID = 0,
    SHARED_QUEUE_STATE_STARTING = 1,
    SHARED_QUEUE_STATE_READY = 2,
    SHARED_QUEUE_STATE_STOPPING = 3,
};

struct queue_header {
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    volatile uint32_t state;
    uint32_t offsets[3];
    uint32_t type;
    uint32_t cx;
    uint32_t cy;
    uint64_t interval;
    uint32_t reserved[8];
};

struct VirtualCameraState {
    HANDLE handle = nullptr;
    queue_header* header = nullptr;
    uint64_t* ts[3] = { nullptr, nullptr, nullptr };
    uint8_t* frame[3] = { nullptr, nullptr, nullptr };
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t capacityWidth = 0;
    uint32_t capacityHeight = 0;
    uint32_t frameCapacityBytes = 0;
    uint64_t interval = 10000000ULL / kVirtualCameraDefaultFps;
    LARGE_INTEGER lastFrameTime = {};
    LARGE_INTEGER perfFreq = {};
    bool active = false;
};

static VirtualCameraState g_vcState;

static void ForceVirtualCameraCaptureFrames(int frameCount);


// Helper to clamp int to byte range (avoids Windows min/max macro conflict)
static inline uint8_t clampToByte(int32_t val) {
    if (val < 0) return 0;
    if (val > 255) return 255;
    return static_cast<uint8_t>(val);
}

enum class VirtualCameraColorSpaceMode {
    Bt601,
    Bt709,
};

static VirtualCameraColorSpaceMode GetVirtualCameraColorSpaceMode(uint32_t width, uint32_t height) {
    return (width >= 1280 || height > 576) ? VirtualCameraColorSpaceMode::Bt709 : VirtualCameraColorSpaceMode::Bt601;
}

static int GetVirtualCameraTargetFps() {
    auto cfgSnapshot = GetConfigSnapshot();
    if (cfgSnapshot && cfgSnapshot->limitCaptureFramerate) {
        return kVirtualCameraLimitedFps;
    }
    return kVirtualCameraDefaultFps;
}

static uint64_t GetVirtualCameraInterval100ns(int fps) {
    if (fps <= 0) { return 10000000ULL / kVirtualCameraDefaultFps; }
    return 10000000ULL / static_cast<uint64_t>(fps);
}

static LONGLONG GetVirtualCameraMinTicks() {
    const int targetFps = GetVirtualCameraTargetFps();
    if (targetFps <= 0 || g_vcState.perfFreq.QuadPart <= 0) { return 0; }
    return (std::max<LONGLONG>)(1, g_vcState.perfFreq.QuadPart / static_cast<LONGLONG>(targetFps));
}

static void SyncVirtualCameraIntervalLocked() {
    const uint64_t targetInterval = GetVirtualCameraInterval100ns(GetVirtualCameraTargetFps());
    if (g_vcState.interval == targetInterval) { return; }

    g_vcState.interval = targetInterval;
    if (g_vcState.header) {
        g_vcState.header->interval = targetInterval;
    }
}


// Record pending resize; actual resize is debounced and applied by FlushPendingVirtualCameraResize().
void OnGameWindowResized(uint32_t newWidth, uint32_t newHeight) {
    if (!IsVirtualCameraActive()) { return; }
    if ((newWidth & 1U) != 0) { newWidth -= 1; }
    if ((newHeight & 1U) != 0) { newHeight -= 1; }
    if (newWidth < 2 || newHeight < 2) { return; }

    g_vcPendingResizeWidth.store(newWidth, std::memory_order_relaxed);
    g_vcPendingResizeHeight.store(newHeight, std::memory_order_relaxed);
    g_vcPendingResizeRequestedMs.store(GetTickCount64(), std::memory_order_release);
}

// Internal: full stop-and-restart reinit. Used when in-place resize is not possible.
static bool ReinitializeVirtualCamera(uint32_t width, uint32_t height) {
    if ((width & 1U) != 0) { width -= 1; }
    if ((height & 1U) != 0) { height -= 1; }
    if (width < 2 || height < 2) { return false; }

    if (IsVirtualCameraActive()) { StopVirtualCamera(); }

    if (!StartVirtualCamera(width, height)) {
        Log("Virtual Camera: Reinit after resize failed - {}", g_vcLastError);
        return false;
    }

    ResetSameThreadVirtualCameraCaptureState();
    return true;
}

bool FlushPendingVirtualCameraResize() {
    const ULONGLONG requestedMs = g_vcPendingResizeRequestedMs.load(std::memory_order_acquire);
    if (requestedMs == 0) { return false; }

    const ULONGLONG now = GetTickCount64();
    if ((now - requestedMs) < kVirtualCameraResizeDebounceMs) { return false; }

    // Consume the pending resize
    const uint32_t width = g_vcPendingResizeWidth.load(std::memory_order_relaxed);
    const uint32_t height = g_vcPendingResizeHeight.load(std::memory_order_relaxed);
    g_vcPendingResizeRequestedMs.store(0, std::memory_order_release);

    if (width < 2 || height < 2) { return false; }
    if (!IsVirtualCameraActive()) { return false; }

    // Check if already at the desired size
    uint32_t currentW = 0, currentH = 0;
    if (GetVirtualCameraResolution(currentW, currentH) && currentW == width && currentH == height) {
        return false;
    }

    return EnsureVirtualCameraSize(width, height);
}

void RequestVirtualCameraRecoveryFrames() {
    std::lock_guard<std::mutex> lock(g_vcMutex);
    g_vcLastCaptureTick.store(0, std::memory_order_release);
    g_vcState.lastFrameTime.QuadPart = 0;
    ForceVirtualCameraCaptureFrames(kVirtualCameraForcedFramesAfterReinit);
}

static void ResolveVirtualCameraMonitorSize(int& outWidth, int& outHeight) {
    outWidth = 0;
    outHeight = 0;

    HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
    if (!GetMonitorSizeForWindow(hwnd, outWidth, outHeight) || outWidth <= 0 || outHeight <= 0) {
        outWidth = GetSystemMetrics(SM_CXSCREEN);
        outHeight = GetSystemMetrics(SM_CYSCREEN);
    }

    if (outWidth < 2) { outWidth = 2; }
    if (outHeight < 2) { outHeight = 2; }
    if ((outWidth & 1) != 0) { --outWidth; }
    if ((outHeight & 1) != 0) { --outHeight; }
}

static uint32_t ResolveVirtualCameraDimension(int configuredValue, int monitorExtent) {
    int resolved = configuredValue;
    if (resolved <= 0) { resolved = monitorExtent; }
    if (monitorExtent > 0) { resolved = (std::min)(resolved, monitorExtent); }
    if (resolved < 2) { resolved = 2; }
    if ((resolved & 1) != 0) { --resolved; }
    return static_cast<uint32_t>(resolved);
}

static void ResolveVirtualCameraAllocationSize(uint32_t requestedWidth,
                                               uint32_t requestedHeight,
                                               uint32_t& outAllocWidth,
                                               uint32_t& outAllocHeight) {
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    if (screenWidth < 2) { screenWidth = 2; }
    if (screenHeight < 2) { screenHeight = 2; }

    outAllocWidth = (std::max)(requestedWidth, static_cast<uint32_t>(screenWidth));
    outAllocHeight = (std::max)(requestedHeight, static_cast<uint32_t>(screenHeight));
    if ((outAllocWidth & 1U) != 0) { --outAllocWidth; }
    if ((outAllocHeight & 1U) != 0) { --outAllocHeight; }
}

static void ForceVirtualCameraCaptureFrames(int frameCount) {
    if (frameCount <= 0) { return; }

    int observed = g_vcForcedCaptureFrames.load(std::memory_order_relaxed);
    while (observed < frameCount &&
           !g_vcForcedCaptureFrames.compare_exchange_weak(observed, frameCount, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed)) {}
}

static void FillVirtualCameraFrameBlack(uint8_t* frameData, uint32_t width, uint32_t height, uint32_t frameCapacityBytes) {
    if (!frameData || width < 2 || height < 2) { return; }

    const uint32_t yPlaneSize = width * height;
    const uint32_t uvPlaneSize = yPlaneSize / 2;
    const uint32_t requiredFrameBytes = yPlaneSize + uvPlaneSize;
    if (requiredFrameBytes > frameCapacityBytes) { return; }

    memset(frameData, 0, frameCapacityBytes);
    memset(frameData, 16, yPlaneSize);
    memset(frameData + yPlaneSize, 128, uvPlaneSize);
}

static void PublishBlankVirtualCameraFrameLocked(uint32_t width, uint32_t height) {
    if (!g_vcState.active || !g_vcState.header || width < 2 || height < 2) { return; }

    const uint32_t requiredFrameBytes = width * height * 3 / 2;
    if (requiredFrameBytes > g_vcState.frameCapacityBytes) { return; }

    if (g_vcState.frame[0]) {
        FillVirtualCameraFrameBlack(g_vcState.frame[0], width, height, g_vcState.frameCapacityBytes);
    }

    g_vcState.header->cx = width;
    g_vcState.header->cy = height;
    g_vcState.header->interval = g_vcState.interval;
    g_vcState.header->write_idx = 0;
    g_vcState.header->read_idx = 0;
    g_vcState.header->state = SHARED_QUEUE_STATE_READY;
    if (g_vcState.ts[0]) { *g_vcState.ts[0] = 0; }

    MemoryBarrier();
}

static bool ResetVirtualCameraStateLocked(uint32_t width, uint32_t height, const char* reason) {
    if (!g_vcState.active || !g_vcState.header || width < 2 || height < 2) { return false; }

    const uint32_t requiredFrameBytes = width * height * 3 / 2;
    if (requiredFrameBytes > g_vcState.frameCapacityBytes) { return false; }

    g_vcState.width = width;
    g_vcState.height = height;
    g_vcState.lastFrameTime.QuadPart = 0;
    g_vcLastCaptureTick.store(0, std::memory_order_release);
    SyncVirtualCameraIntervalLocked();

    g_vcState.header->cx = width;
    g_vcState.header->cy = height;
    g_vcState.header->interval = g_vcState.interval;

    for (int i = 0; i < 3; ++i) {
        if (g_vcState.ts[i]) { *g_vcState.ts[i] = 0; }
        FillVirtualCameraFrameBlack(g_vcState.frame[i], width, height, g_vcState.frameCapacityBytes);
    }

    PublishBlankVirtualCameraFrameLocked(width, height);
    ForceVirtualCameraCaptureFrames(kVirtualCameraForcedFramesAfterReinit);

    MemoryBarrier();

    g_vcLastError.clear();
    Log("Virtual Camera: Reinitialized {} at {}x{}", reason, width, height);
    return true;
}

// Single pass: computes Y for every pixel and UV for every 2x2 block simultaneously
static void ConvertRGBAtoNV12(const uint8_t* __restrict rgba, uint8_t* __restrict nv12, uint32_t width, uint32_t height) {
    const uint32_t yPlaneSize = width * height;
    uint8_t* __restrict yPlane = nv12;
    uint8_t* __restrict uvPlane = nv12 + yPlaneSize;
    const uint32_t stride = width * 4;
    const VirtualCameraColorSpaceMode colorSpaceMode = GetVirtualCameraColorSpaceMode(width, height);

    const int32_t yR = colorSpaceMode == VirtualCameraColorSpaceMode::Bt709 ? 47 : 66;
    const int32_t yG = colorSpaceMode == VirtualCameraColorSpaceMode::Bt709 ? 157 : 129;
    const int32_t yB = colorSpaceMode == VirtualCameraColorSpaceMode::Bt709 ? 16 : 25;
    const int32_t uR = colorSpaceMode == VirtualCameraColorSpaceMode::Bt709 ? -26 : -38;
    const int32_t uG = colorSpaceMode == VirtualCameraColorSpaceMode::Bt709 ? -87 : -74;
    const int32_t uB = 112;
    const int32_t vR = 112;
    const int32_t vG = colorSpaceMode == VirtualCameraColorSpaceMode::Bt709 ? -102 : -94;
    const int32_t vB = colorSpaceMode == VirtualCameraColorSpaceMode::Bt709 ? -10 : -18;

    for (uint32_t y = 0; y < height; y += 2) {
        const uint8_t* __restrict srcRow0 = rgba + (height - 1 - y) * stride;
        const uint8_t* __restrict srcRow1 = rgba + (height - 2 - y) * stride;
        uint8_t* __restrict yRow0 = yPlane + y * width;
        uint8_t* __restrict yRow1 = yPlane + (y + 1) * width;
        uint8_t* __restrict uvRow = uvPlane + (y / 2) * width;

        for (uint32_t x = 0; x < width; x += 2) {
            // Load 2x2 block of RGBA pixels
            const uint8_t* p00 = srcRow0 + x * 4;
            const uint8_t* p10 = srcRow0 + (x + 1) * 4;
            const uint8_t* p01 = srcRow1 + x * 4;
            const uint8_t* p11 = srcRow1 + (x + 1) * 4;

            int32_t y00 = ((yR * p00[0] + yG * p00[1] + yB * p00[2] + 128) >> 8) + 16;
            int32_t y10 = ((yR * p10[0] + yG * p10[1] + yB * p10[2] + 128) >> 8) + 16;
            int32_t y01 = ((yR * p01[0] + yG * p01[1] + yB * p01[2] + 128) >> 8) + 16;
            int32_t y11 = ((yR * p11[0] + yG * p11[1] + yB * p11[2] + 128) >> 8) + 16;

            yRow0[x] = clampToByte(y00);
            yRow0[x + 1] = clampToByte(y10);
            yRow1[x] = clampToByte(y01);
            yRow1[x + 1] = clampToByte(y11);

            // Average RGB of 2x2 block for chroma (shift right by 2 = divide by 4)
            int32_t avgR = (p00[0] + p10[0] + p01[0] + p11[0] + 2) >> 2;
            int32_t avgG = (p00[1] + p10[1] + p01[1] + p11[1] + 2) >> 2;
            int32_t avgB = (p00[2] + p10[2] + p01[2] + p11[2] + 2) >> 2;

            int32_t u = ((uR * avgR + uG * avgG + uB * avgB + 128) >> 8) + 128;
            int32_t v = ((vR * avgR + vG * avgG + vB * avgB + 128) >> 8) + 128;

            uvRow[x] = clampToByte(u);
            uvRow[x + 1] = clampToByte(v);
        }
    }
}

bool IsVirtualCameraDriverInstalled() {
    HKEY hKey;
    LONG result = RegOpenKeyExA(HKEY_CLASSES_ROOT, "CLSID\\{A3FCE0F5-3493-419F-958A-ABA1250EC20B}", 0, KEY_READ, &hKey);

    if (result == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }

    const char* possiblePaths[] = {
        "C:\\Program Files\\obs-studio\\data\\obs-plugins\\win-dshow\\obs-virtualcam-module64.dll",
        "C:\\Program Files (x86)\\obs-studio\\data\\obs-plugins\\win-dshow\\obs-virtualcam-module64.dll",
    };

    for (const auto& path : possiblePaths) {
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) { return true; }
    }

    return false;
}

bool IsVirtualCameraInUseByOBS() {
    if (g_virtualCameraActive.load(std::memory_order_acquire)) { return false; }

    HANDLE testHandle = OpenFileMappingW(FILE_MAP_READ, FALSE, VIDEO_NAME);
    if (!testHandle) {
        return false;
    }

    queue_header* testHeader = static_cast<queue_header*>(MapViewOfFile(testHandle, FILE_MAP_READ, 0, 0, sizeof(queue_header)));

    bool inUse = false;
    if (testHeader) {
        inUse = (testHeader->state == SHARED_QUEUE_STATE_READY || testHeader->state == SHARED_QUEUE_STATE_STARTING);
        UnmapViewOfFile(testHeader);
    }

    CloseHandle(testHandle);
    return inUse;
}

void GetVirtualCameraMonitorSize(uint32_t& outWidth, uint32_t& outHeight) {
    int monitorWidth = 0;
    int monitorHeight = 0;
    ResolveVirtualCameraMonitorSize(monitorWidth, monitorHeight);
    outWidth = static_cast<uint32_t>(monitorWidth);
    outHeight = static_cast<uint32_t>(monitorHeight);
}

bool GetPreferredVirtualCameraResolution(uint32_t& outWidth, uint32_t& outHeight) {
    outWidth = 0;
    outHeight = 0;

    int monitorWidth = 0;
    int monitorHeight = 0;
    ResolveVirtualCameraMonitorSize(monitorWidth, monitorHeight);

    uint32_t width = ResolveVirtualCameraDimension(0, monitorWidth);
    uint32_t height = ResolveVirtualCameraDimension(0, monitorHeight);
    if (width < 2 || height < 2) { return false; }

    outWidth = width;
    outHeight = height;
    return true;
}

bool StartVirtualCamera(uint32_t width, uint32_t height) {
    if ((width & 1U) != 0) { width -= 1; }
    if ((height & 1U) != 0) { height -= 1; }
    if (width < 2 || height < 2) {
        g_vcLastError = "Invalid virtual camera dimensions";
        Log("Virtual Camera: {}", g_vcLastError);
        return false;
    }

    std::lock_guard<std::mutex> lock(g_vcMutex);

    if (g_vcState.active) {
        g_vcLastError = "Virtual camera already active";
        return true;
    }

    if (!IsVirtualCameraDriverInstalled()) {
        g_vcLastError = "OBS Virtual Camera driver not installed";
        Log("Virtual Camera: {}", g_vcLastError);
        return false;
    }

    if (IsVirtualCameraInUseByOBS()) {
        g_vcLastError = "Virtual camera is currently in use by OBS";
        Log("Virtual Camera: {}", g_vcLastError);
        return false;
    }

    const int targetFps = GetVirtualCameraTargetFps();
    g_vcState.interval = GetVirtualCameraInterval100ns(targetFps);
    QueryPerformanceFrequency(&g_vcState.perfFreq);
    g_vcState.lastFrameTime.QuadPart = 0;
    g_vcLastCaptureTick.store(0, std::memory_order_release);

    uint32_t allocWidth = 0;
    uint32_t allocHeight = 0;
    ResolveVirtualCameraAllocationSize(width, height, allocWidth, allocHeight);

    uint32_t frameSize = allocWidth * allocHeight * 3 / 2;
    uint32_t offset_frame[3];
    uint32_t totalSize;

    totalSize = sizeof(queue_header);
    ALIGN_SIZE(totalSize, 32);

    offset_frame[0] = totalSize;
    totalSize += frameSize + FRAME_HEADER_SIZE;
    ALIGN_SIZE(totalSize, 32);

    offset_frame[1] = totalSize;
    totalSize += frameSize + FRAME_HEADER_SIZE;
    ALIGN_SIZE(totalSize, 32);

    offset_frame[2] = totalSize;
    totalSize += frameSize + FRAME_HEADER_SIZE;
    ALIGN_SIZE(totalSize, 32);

    g_vcState.handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, totalSize, VIDEO_NAME);

    if (!g_vcState.handle) {
        g_vcLastError = "Failed to create shared memory (error " + std::to_string(GetLastError()) + ")";
        Log("Virtual Camera: {}", g_vcLastError);
        return false;
    }

    g_vcState.header = static_cast<queue_header*>(MapViewOfFile(g_vcState.handle, FILE_MAP_ALL_ACCESS, 0, 0, 0));

    if (!g_vcState.header) {
        CloseHandle(g_vcState.handle);
        g_vcState.handle = nullptr;
        g_vcLastError = "Failed to map shared memory";
        Log("Virtual Camera: {}", g_vcLastError);
        return false;
    }

    memset(g_vcState.header, 0, sizeof(queue_header));
    g_vcState.header->state = SHARED_QUEUE_STATE_STARTING;
    g_vcState.header->type = 0;
    g_vcState.header->cx = width;
    g_vcState.header->cy = height;
    g_vcState.header->interval = g_vcState.interval;

    for (int i = 0; i < 3; i++) {
        g_vcState.header->offsets[i] = offset_frame[i];
        uint8_t* basePtr = reinterpret_cast<uint8_t*>(g_vcState.header);
        g_vcState.ts[i] = reinterpret_cast<uint64_t*>(basePtr + offset_frame[i]);
        g_vcState.frame[i] = basePtr + offset_frame[i] + FRAME_HEADER_SIZE;
    }

    g_vcState.width = width;
    g_vcState.height = height;
    g_vcState.capacityWidth = allocWidth;
    g_vcState.capacityHeight = allocHeight;
    g_vcState.frameCapacityBytes = frameSize;
    g_vcState.active = true;
    g_virtualCameraActive.store(true, std::memory_order_release);
    g_vcLastError.clear();
    for (int i = 0; i < 3; ++i) {
        if (g_vcState.ts[i]) { *g_vcState.ts[i] = 0; }
        FillVirtualCameraFrameBlack(g_vcState.frame[i], width, height, g_vcState.frameCapacityBytes);
    }
    PublishBlankVirtualCameraFrameLocked(width, height);
    ForceVirtualCameraCaptureFrames(kVirtualCameraForcedFramesAfterReinit);

    Log("Virtual Camera: Started at {}x{} @ {}fps", width, height, targetFps);
    return true;
}

void StopVirtualCamera() {
    g_virtualCameraActive.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_vcMutex);

    if (!g_vcState.active) { return; }

    if (g_vcState.header) { g_vcState.header->state = SHARED_QUEUE_STATE_STOPPING; }

    if (g_vcState.header) {
        UnmapViewOfFile(g_vcState.header);
        g_vcState.header = nullptr;
    }

    if (g_vcState.handle) {
        CloseHandle(g_vcState.handle);
        g_vcState.handle = nullptr;
    }

    for (int i = 0; i < 3; i++) {
        g_vcState.ts[i] = nullptr;
        g_vcState.frame[i] = nullptr;
    }

    g_vcState.active = false;
    g_vcState.width = 0;
    g_vcState.height = 0;
    g_vcState.capacityWidth = 0;
    g_vcState.capacityHeight = 0;
    g_vcState.frameCapacityBytes = 0;
    g_vcState.lastFrameTime.QuadPart = 0;
    g_vcLastCaptureTick.store(0, std::memory_order_release);
    g_vcForcedCaptureFrames.store(0, std::memory_order_release);

    Log("Virtual Camera: Stopped");
}

bool ShouldCaptureVirtualCameraFrame() {
    if (!g_virtualCameraActive.load(std::memory_order_acquire)) { return false; }

    int forcedFrames = g_vcForcedCaptureFrames.load(std::memory_order_relaxed);
    while (forcedFrames > 0) {
        if (g_vcForcedCaptureFrames.compare_exchange_weak(forcedFrames, forcedFrames - 1, std::memory_order_acq_rel,
                                                          std::memory_order_relaxed)) {
            return true;
        }
    }

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    const LONGLONG minTicks = GetVirtualCameraMinTicks();
    if (minTicks <= 0) { return true; }

    LONGLONG observed = g_vcLastCaptureTick.load(std::memory_order_relaxed);
    while (true) {
        if (observed != 0 && (now.QuadPart - observed) < minTicks) { return false; }
        if (g_vcLastCaptureTick.compare_exchange_weak(observed, now.QuadPart, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return true;
        }
    }
}

bool EnsureVirtualCameraSize(uint32_t width, uint32_t height) {
    if ((width & 1U) != 0) { width -= 1; }
    if ((height & 1U) != 0) { height -= 1; }
    if (width < 2 || height < 2) { return false; }

    {
        std::lock_guard<std::mutex> lock(g_vcMutex);
        if (!g_vcState.active) { return false; }
        if (g_vcState.width == width && g_vcState.height == height) { return true; }
        if (ResetVirtualCameraStateLocked(width, height, "for size change")) {
            ResetSameThreadVirtualCameraCaptureState();
            return true;
        }
    }

    Log("Virtual Camera: Size change exceeds current shared-memory capacity, recreating producer");

    StopVirtualCamera();
    if (!StartVirtualCamera(width, height)) {
        Log("Virtual Camera: Resize failed - {}", g_vcLastError);
        return false;
    }

    ResetSameThreadVirtualCameraCaptureState();
    return true;
}



bool GetVirtualCameraResolution(uint32_t& outWidth, uint32_t& outHeight) {
    std::lock_guard<std::mutex> lock(g_vcMutex);
    if (!g_vcState.active) {
        outWidth = 0;
        outHeight = 0;
        return false;
    }

    outWidth = g_vcState.width;
    outHeight = g_vcState.height;
    return true;
}

bool WriteVirtualCameraFrame(const uint8_t* rgba_data, uint32_t width, uint32_t height, uint64_t timestamp) {
    if (!g_virtualCameraActive.load(std::memory_order_acquire)) { return false; }

    std::lock_guard<std::mutex> lock(g_vcMutex);

    if (!g_vcState.active || !g_vcState.header) { return false; }
    SyncVirtualCameraIntervalLocked();

    // FPS limiting before any work (lock-free fast path)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_vcState.lastFrameTime.QuadPart != 0) {
        LONGLONG elapsed = now.QuadPart - g_vcState.lastFrameTime.QuadPart;
        LONGLONG minTicks = GetVirtualCameraMinTicks();
        if (elapsed < minTicks) {
            return true;
        }
    }

    if (width != g_vcState.width || height != g_vcState.height) { return false; }

    uint32_t writeIdx = g_vcState.header->write_idx + 1;
    uint32_t idx = writeIdx % 3;
    uint8_t* dst = g_vcState.frame[idx];

    ConvertRGBAtoNV12(rgba_data, dst, width, height);

    *g_vcState.ts[idx] = timestamp;

    MemoryBarrier();

    // Update indices atomically
    g_vcState.header->write_idx = writeIdx;
    g_vcState.header->read_idx = writeIdx;
    g_vcState.header->state = SHARED_QUEUE_STATE_READY;

    MemoryBarrier();

    static int frameCount = 0;
    if (frameCount < 3) {
        uint32_t frameSize = width * height * 3 / 2;
        Log("Virtual Camera: Wrote frame {} at idx {} ts={} size={}", frameCount, idx, timestamp, frameSize);
        frameCount++;
    }

    g_vcState.lastFrameTime = now;
    return true;
}

bool WriteVirtualCameraFrameNV12(const uint8_t* nv12_data, uint32_t width, uint32_t height, uint64_t timestamp) {
    if (!nv12_data) { return false; }

    const size_t yPlaneSize = static_cast<size_t>(width) * static_cast<size_t>(height);
    return WriteVirtualCameraFrameNV12Planes(nv12_data, nv12_data + yPlaneSize, width, height, timestamp);
}

bool WriteVirtualCameraFrameNV12Planes(const uint8_t* y_plane, const uint8_t* uv_plane, uint32_t width, uint32_t height,
                                       uint64_t timestamp) {
    if (!g_virtualCameraActive.load(std::memory_order_acquire)) { return false; }
    if (!y_plane || !uv_plane) { return false; }

    std::lock_guard<std::mutex> lock(g_vcMutex);

    if (!g_vcState.active || !g_vcState.header) { return false; }
    SyncVirtualCameraIntervalLocked();

    // FPS limiting (lock-free integer comparison)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_vcState.lastFrameTime.QuadPart != 0) {
        LONGLONG elapsed = now.QuadPart - g_vcState.lastFrameTime.QuadPart;
        LONGLONG minTicks = GetVirtualCameraMinTicks();
        if (elapsed < minTicks) {
            return true;
        }
    }

    if (width != g_vcState.width || height != g_vcState.height) { return false; }

    uint32_t writeIdx = g_vcState.header->write_idx + 1;
    uint32_t idx = writeIdx % 3;

    const size_t yPlaneSize = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t uvPlaneSize = yPlaneSize / 2u;
    memcpy(g_vcState.frame[idx], y_plane, yPlaneSize);
    memcpy(g_vcState.frame[idx] + yPlaneSize, uv_plane, uvPlaneSize);

    *g_vcState.ts[idx] = timestamp;

    MemoryBarrier();

    g_vcState.header->write_idx = writeIdx;
    g_vcState.header->read_idx = writeIdx;
    g_vcState.header->state = SHARED_QUEUE_STATE_READY;

    MemoryBarrier();

    g_vcState.lastFrameTime = now;
    return true;
}

bool IsVirtualCameraActive() { return g_virtualCameraActive.load(std::memory_order_acquire); }

const char* GetVirtualCameraError() { return g_vcLastError.c_str(); }


