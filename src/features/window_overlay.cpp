#include "window_overlay.h"
#include "gui/gui.h"
#include "common/profiler.h"
#include "render/render.h"
#include "common/utils.h"
#include <GL/wglew.h>
#include <algorithm>
#include <cmath>
#include <dwmapi.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <tuple>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "msimg32.lib")

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 14
#endif

// Global variables for window overlay cache and thread management
std::map<std::string, std::unique_ptr<WindowOverlayCacheEntry>> g_windowOverlayCache;
std::mutex g_windowOverlayCacheMutex;

std::atomic<bool> g_stopWindowCaptureThread{ false };
std::thread g_windowCaptureThread;

// Global window list cache for GUI (prevents UI blocking from expensive window enumeration)
std::atomic<std::vector<WindowInfo>*> g_windowListCache{ nullptr };
std::mutex g_windowListCacheMutex;
std::chrono::steady_clock::time_point g_lastWindowListUpdate;

// Deferred overlay reload queue (for GUI thread to request reloads without blocking)
struct DeferredOverlayReload {
    std::string overlayId;
    WindowOverlayConfig config;
};
std::vector<DeferredOverlayReload> g_deferredOverlayReloads;
std::mutex g_deferredOverlayReloadsMutex;

WindowOverlayCacheEntry::~WindowOverlayCacheEntry() {
    if (hBitmap) {
        DeleteObject(hBitmap);
        hBitmap = NULL;
    }
    if (hdcMem) {
        DeleteDC(hdcMem);
        hdcMem = NULL;
    }
    if (pixelData) {
        delete[] pixelData;
        pixelData = nullptr;
    }
    // Note: OpenGL texture cleanup should be done on the OpenGL thread
}

std::string GetExecutableNameFromWindow(HWND hwnd);

HWND FindWindowByTitleAndClass(const std::string& title, const std::string& className, const std::string& executableName = "",
                               const std::string& matchPriority = "title") {
    struct EnumData {
        std::string targetTitle;
        std::string targetClass;
        std::string targetExecutable;
        std::string matchPriority;
        HWND exactMatch = nullptr;
        HWND classMatch = nullptr;
        HWND executableMatch = nullptr;
    };

    EnumData data{ title, className, executableName, matchPriority };

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            EnumData* data = reinterpret_cast<EnumData*>(lParam);

            HWND gameHwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
            if (gameHwnd && hwnd == gameHwnd) { return TRUE; }
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != 0 && pid == GetCurrentProcessId()) { return TRUE; }

            // Skip invisible windows
            if (!IsWindowVisible(hwnd)) { return TRUE; }

            char windowTitle[256];
            GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle));
            std::string title = windowTitle;

            char windowClass[256];
            GetClassNameA(hwnd, windowClass, sizeof(windowClass));
            std::string className = windowClass;

            std::string exeName = GetExecutableNameFromWindow(hwnd);

            bool titleMatch = !data->targetTitle.empty() && title == data->targetTitle;

            bool classMatch = !data->targetClass.empty() && className == data->targetClass;

            bool executableMatch = !data->targetExecutable.empty() && exeName == data->targetExecutable;

            if (titleMatch) {
                data->exactMatch = hwnd;
                return FALSE;
            }

            if (classMatch && data->classMatch == NULL) { data->classMatch = hwnd; }

            if (executableMatch && data->executableMatch == NULL) { data->executableMatch = hwnd; }

            return TRUE;
        },
        reinterpret_cast<LPARAM>(&data));

    if (data.exactMatch) { return data.exactMatch; }

    if (matchPriority == "title_class" && data.classMatch) { return data.classMatch; }

    if (matchPriority == "title_executable" && data.executableMatch) { return data.executableMatch; }

    return NULL;
}

std::atomic<bool> g_windowOverlaysInitialized{ false };

static void ForceWindowOverlayRedraw(HWND targetHwnd) {
    if (!targetHwnd || !IsWindow(targetHwnd)) { return; }

    InvalidateRect(targetHwnd, NULL, FALSE);
    RedrawWindow(targetHwnd, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);

    constexpr UINT kForceUpdateMessageTimeoutMs = 16;
    SendMessageTimeoutW(targetHwnd, WM_PAINT, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, kForceUpdateMessageTimeoutMs, nullptr);
    SendMessageTimeoutW(targetHwnd, WM_CAPTURECHANGED, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, kForceUpdateMessageTimeoutMs, nullptr);
}

// NOTE: This is called from the window capture background thread
// to avoid blocking the render thread during expensive window searching
void InitializeWindowOverlays() {
    // Use config snapshot for thread-safe access (called from background thread)
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) {
        Log("No config snapshot available for window overlay initialization, skipping");
        return;
    }

    if (cfgSnap->windowOverlays.empty()) {
        Log("No window overlays configured, skipping initialization");
        return;
    }

    for (const auto& config : cfgSnap->windowOverlays) {
        try {
            LoadWindowOverlay(config.name, config);
        } catch (const std::exception& e) { Log("Error loading window overlay '" + config.name + "': " + e.what()); } catch (...) {
            Log("Unknown error loading window overlay '" + config.name + "'");
        }
    }

    Log("Initialized " + std::to_string(cfgSnap->windowOverlays.size()) + " window overlays");
}

// Internal helper - updates entry without acquiring lock (caller must hold lock)
static void LoadWindowOverlay_Internal(const std::string& overlayId, const WindowOverlayConfig& config) {
    // Note: Caller must hold g_windowOverlayCacheMutex

    auto it = g_windowOverlayCache.find(overlayId);
    if (it != g_windowOverlayCache.end()) {
        auto& entry = it->second;

        bool windowChanged = (entry->windowTitle != config.windowTitle || entry->windowClass != config.windowClass ||
                              entry->executableName != config.executableName || entry->windowMatchPriority != config.windowMatchPriority);

        entry->windowTitle = config.windowTitle;
        entry->windowClass = config.windowClass;
        entry->executableName = config.executableName;
        entry->windowMatchPriority = config.windowMatchPriority;
        entry->fps.store(config.fps, std::memory_order_relaxed);
        entry->searchInterval.store(config.searchInterval, std::memory_order_relaxed);
        entry->needsUpdate.store(true, std::memory_order_relaxed);

        if (windowChanged) {
            entry->targetWindow.store(
                FindWindowByTitleAndClass(config.windowTitle, config.windowClass, config.executableName, config.windowMatchPriority),
                std::memory_order_relaxed);

            if (entry->targetWindow.load(std::memory_order_relaxed)) {
                Log("Updated target window for overlay '" + overlayId + "': " + config.windowTitle);
            } else {
                Log("Warning: Could not find target window for overlay '" + overlayId + "': " + config.windowTitle);
            }
        }

        return;
    }

    auto entry = std::make_unique<WindowOverlayCacheEntry>();
    entry->windowTitle = config.windowTitle;
    entry->windowClass = config.windowClass;
    entry->executableName = config.executableName;
    entry->windowMatchPriority = config.windowMatchPriority;
    entry->fps.store(config.fps, std::memory_order_relaxed);
    entry->searchInterval.store(config.searchInterval, std::memory_order_relaxed);
    entry->needsUpdate.store(true, std::memory_order_relaxed);

    entry->targetWindow.store(
        FindWindowByTitleAndClass(config.windowTitle, config.windowClass, config.executableName, config.windowMatchPriority),
        std::memory_order_relaxed);

    if (entry->targetWindow.load(std::memory_order_relaxed)) {
        Log("Found target window for overlay '" + overlayId + "': " + config.windowTitle);
    } else {
        Log("Warning: Could not find target window for overlay '" + overlayId + "': " + config.windowTitle);
    }

    g_windowOverlayCache[overlayId] = std::move(entry);
}

void LoadWindowOverlay(const std::string& overlayId, const WindowOverlayConfig& config) {
    std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
    LoadWindowOverlay_Internal(overlayId, config);
}

// Queue a deferred overlay reload (non-blocking, safe to call from GUI thread)
void QueueOverlayReload(const std::string& overlayId, const WindowOverlayConfig& config) {
    std::lock_guard<std::mutex> lock(g_deferredOverlayReloadsMutex);
    for (auto& pending : g_deferredOverlayReloads) {
        if (pending.overlayId == overlayId) {
            pending.config = config;
            return;
        }
    }
    g_deferredOverlayReloads.push_back({ overlayId, config });
}

void UpdateAllWindowOverlays() {
    auto now = std::chrono::steady_clock::now();
    struct PendingWindowSearch {
        std::string overlayId;
        std::string windowTitle;
        std::string windowClass;
        std::string executableName;
        std::string windowMatchPriority;
    };
    std::vector<PendingWindowSearch> searchesToRun;

    {
        std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);

        for (auto& [overlayId, entry] : g_windowOverlayCache) {
            HWND target = entry->targetWindow.load(std::memory_order_relaxed);
            if (target && !IsWindow(target)) {
                entry->targetWindow.store(NULL, std::memory_order_relaxed);
                entry->lastSearchTime = now;
                target = NULL;
            }

            if (!target) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry->lastSearchTime);
                int interval = entry->searchInterval.load(std::memory_order_relaxed);

                if (elapsed.count() >= interval) {
                    searchesToRun.push_back(
                        { overlayId, entry->windowTitle, entry->windowClass, entry->executableName, entry->windowMatchPriority });
                    entry->lastSearchTime = now;
                }
            }
        }
    }

    for (const auto& search : searchesToRun) {
        HWND found = FindWindowByTitleAndClass(search.windowTitle, search.windowClass, search.executableName, search.windowMatchPriority);

        std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
        auto it = g_windowOverlayCache.find(search.overlayId);
        if (it == g_windowOverlayCache.end() || !it->second) { continue; }

        WindowOverlayCacheEntry& entry = *it->second;
        if (entry.windowTitle != search.windowTitle || entry.windowClass != search.windowClass ||
            entry.executableName != search.executableName || entry.windowMatchPriority != search.windowMatchPriority) {
            continue;
        }

        entry.targetWindow.store(found, std::memory_order_relaxed);

        if (found) {
            Log("Reacquired target window for overlay '" + search.overlayId + "'");
            entry.needsUpdate.store(true, std::memory_order_relaxed);
        }
    }
}

void UpdateWindowOverlayFPS(const std::string& overlayId, int newFPS) {
    // LOCK-FREE: fps is atomic, so we just need to get the entry pointer safely
    // We use a very brief lock just to get the pointer, then release immediately
    WindowOverlayCacheEntry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
        auto it = g_windowOverlayCache.find(overlayId);
        if (it != g_windowOverlayCache.end()) { entry = it->second.get(); }
    }
    // Lock released - now we can safely access atomic members

    if (entry) {
        // These are atomic operations, no lock needed
        entry->fps.store(newFPS, std::memory_order_relaxed);
        entry->needsUpdate.store(true, std::memory_order_relaxed);
        Log("Updated FPS for overlay '" + overlayId + "' to " + std::to_string(newFPS));
    } else {
        Log("FPS update requested for overlay '" + overlayId + "' but cache entry not found (overlay may not be loaded yet)");
    }
}

void UpdateWindowOverlaySearchInterval(const std::string& overlayId, int newSearchInterval) {
    // LOCK-FREE: searchInterval is atomic, so we just need to get the entry pointer safely
    // We use a very brief lock just to get the pointer, then release immediately
    WindowOverlayCacheEntry* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
        auto it = g_windowOverlayCache.find(overlayId);
        if (it != g_windowOverlayCache.end()) { entry = it->second.get(); }
    }
    // Lock released - now we can safely access atomic members

    if (entry) {
        // This is an atomic operation, no lock needed
        entry->searchInterval.store(newSearchInterval, std::memory_order_relaxed);
        Log("Updated search interval for overlay '" + overlayId + "' to " + std::to_string(newSearchInterval) + "ms");
    } else {
        Log("Search interval update requested for overlay '" + overlayId + "' but cache entry not found (overlay may not be loaded yet)");
    }
}

// NOTE: This is called from the render thread via GUI. To avoid freezing,
// we only mark the entry for update and let the background thread do the expensive work.
void UpdateWindowOverlay(const std::string& overlayId) {
    std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);

    auto it = g_windowOverlayCache.find(overlayId);
    if (it == g_windowOverlayCache.end()) { return; }

    WindowOverlayCacheEntry& entry = *it->second;

    {
        HWND target = entry.targetWindow.load(std::memory_order_relaxed);
        if (target && !IsWindow(target)) { entry.targetWindow.store(NULL, std::memory_order_relaxed); }
    }

    // Mark for update - the background thread will find the window
    // This avoids expensive window enumeration on the render thread
    entry.needsUpdate.store(true, std::memory_order_relaxed);
    entry.lastSearchTime = std::chrono::steady_clock::now() - std::chrono::seconds(100);
}

bool CaptureWindowContent(WindowOverlayCacheEntry& entry, const WindowOverlayConfig& config) {
    std::lock_guard<std::mutex> lock(entry.captureMutex);

    HWND targetHwnd = entry.targetWindow.load(std::memory_order_relaxed);
    if (!targetHwnd || !IsWindow(targetHwnd) || !IsWindowVisible(targetHwnd)) { return false; }

    {
        HWND gameHwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
        if (gameHwnd && targetHwnd == gameHwnd) {
            Log("[WindowOverlay] Refusing to capture the game window (self-capture). Clearing target.");
            entry.targetWindow.store(NULL, std::memory_order_relaxed);
            entry.needsUpdate.store(true, std::memory_order_relaxed);
            entry.lastSearchTime = std::chrono::steady_clock::now() - std::chrono::seconds(100);
            return false;
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(targetHwnd, &pid);
        if (pid != 0 && pid == GetCurrentProcessId()) {
            Log("[WindowOverlay] Refusing to capture a same-process window (self-capture). Clearing target.");
            entry.targetWindow.store(NULL, std::memory_order_relaxed);
            entry.needsUpdate.store(true, std::memory_order_relaxed);
            entry.lastSearchTime = std::chrono::steady_clock::now() - std::chrono::seconds(100);
            return false;
        }
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - entry.lastCaptureTime);
    int targetInterval = 1000 / std::max(1, entry.fps.load(std::memory_order_relaxed));

    if (elapsed.count() < targetInterval && !entry.needsUpdate.load(std::memory_order_relaxed)) {
        return true;
    }

    entry.lastCaptureTime = now;
    entry.needsUpdate.store(false, std::memory_order_relaxed);

    targetHwnd = entry.targetWindow.load(std::memory_order_relaxed);
    if (!targetHwnd || !IsWindow(targetHwnd)) { return false; }

    if (config.forceUpdate) { ForceWindowOverlayRedraw(targetHwnd); }

    RECT clientRect;
    if (!GetClientRect(targetHwnd, &clientRect)) { return false; }

    int windowWidth = clientRect.right - clientRect.left;
    int windowHeight = clientRect.bottom - clientRect.top;

    if (windowWidth <= 0 || windowHeight <= 0) { return false; }

    int captureWidth = windowWidth;
    int captureHeight = windowHeight;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    if (!hdcScreen || !hdcMem) {
        if (hdcScreen) ReleaseDC(NULL, hdcScreen);
        if (hdcMem) DeleteDC(hdcMem);
        return false;
    }

    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, captureWidth, captureHeight);
    if (!hBitmap) {
        ReleaseDC(NULL, hdcScreen);
        DeleteDC(hdcMem);
        return false;
    }

    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMem, hBitmap);

    // If BitBlt/PrintWindow fails, we don't want uninitialized memory showing other windows
    RECT clearRect = { 0, 0, captureWidth, captureHeight };
    HBRUSH hBlackBrush = (HBRUSH)GetStockObject(DKGRAY_BRUSH);
    FillRect(hdcMem, &clearRect, hBlackBrush);

    int oldROP = SetROP2(hdcMem, R2_COPYPEN);

    BOOL isCloaked = FALSE;
    HRESULT hr = DwmGetWindowAttribute(targetHwnd, DWMWA_CLOAKED, &isCloaked, sizeof(isCloaked));
    bool windowIsCloaked = SUCCEEDED(hr) && isCloaked;
    bool windowIsIconic = IsIconic(targetHwnd);

    // We avoid it for cloaked/iconic windows since they may not render properly
    bool shouldAvoidPrintWindow = windowIsCloaked || windowIsIconic;

    BOOL result = FALSE;
    HDC hdcWindow = NULL;
    bool usedPrintWindow = false;

    if (config.captureMethod == "BitBlt") {
        // Note: BitBlt requires GetDC which CAN cause flicker on some windows
        hdcWindow = GetDC(targetHwnd);
        if (hdcWindow) { result = BitBlt(hdcMem, 0, 0, captureWidth, captureHeight, hdcWindow, 0, 0, SRCCOPY); }
    } else {
        // Windows 10+ method (default): Uses PrintWindow with PW_RENDERFULLCONTENT
        if (!shouldAvoidPrintWindow) {
            result = PrintWindow(targetHwnd, hdcMem, PW_RENDERFULLCONTENT);
            usedPrintWindow = true;
        }
        if (!result) {
            hdcWindow = GetDC(targetHwnd);
            if (hdcWindow) { result = BitBlt(hdcMem, 0, 0, captureWidth, captureHeight, hdcWindow, 0, 0, SRCCOPY); }
        }
    }

    SetROP2(hdcMem, oldROP);

    bool success = false;

    if (entry.width != captureWidth || entry.height != captureHeight) {
        if (entry.pixelData) {
            delete[] entry.pixelData;
            entry.pixelData = nullptr;
        }
        entry.width = captureWidth;
        entry.height = captureHeight;
        size_t bufferSize = static_cast<size_t>(captureWidth) * static_cast<size_t>(captureHeight) * 4;
        if (bufferSize > 0 && bufferSize < 100 * 1024 * 1024) {
            entry.pixelData = new unsigned char[bufferSize];
        } else {
            Log("[WindowOverlay] Invalid buffer size: " + std::to_string(bufferSize));
            SelectObject(hdcMem, hOldBitmap);
            DeleteObject(hBitmap);
            DeleteDC(hdcMem);
            if (hdcWindow) { ReleaseDC(targetHwnd, hdcWindow); }
            ReleaseDC(NULL, hdcScreen);
            return false;
        }
    }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = captureWidth;
    bmi.bmiHeader.biHeight = -captureHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    int scanlines = GetDIBits(hdcScreen, hBitmap, 0, captureHeight, entry.pixelData, &bmi, DIB_RGB_COLORS);

    if (scanlines == captureHeight) {
        const int totalPixels = captureWidth * captureHeight;

        if (result && config.enableColorKey && !config.colorKeys.empty()) {
            for (int i = 0; i < totalPixels; i++) {
                unsigned char* pixel = &entry.pixelData[i * 4];
                std::swap(pixel[0], pixel[2]);

                float r = pixel[0] / 255.0f;
                float g = pixel[1] / 255.0f;
                float b = pixel[2] / 255.0f;

                bool matchesAnyKey = false;
                for (const auto& key : config.colorKeys) {
                    float dr = r - key.color.r;
                    float dg = g - key.color.g;
                    float db = b - key.color.b;
                    float distanceSq = dr * dr + dg * dg + db * db;
                    float sensitivitySq = key.sensitivity * key.sensitivity;

                    if (distanceSq <= sensitivitySq) {
                        matchesAnyKey = true;
                        break;
                    }
                }

                pixel[3] = matchesAnyKey ? 0 : 255;
            }
        } else {
            for (int i = 0; i < totalPixels; i++) {
                unsigned char* pixel = &entry.pixelData[i * 4];
                std::swap(pixel[0], pixel[2]);
                pixel[3] = 255;
            }
        }

        success = true;
    }

    if (success) {
        // Copy to write buffer for thread-safe transfer to render thread (OpenGL path)
        if (entry.writeBuffer->width != entry.width || entry.writeBuffer->height != entry.height) {
            if (entry.writeBuffer->pixelData) {
                delete[] entry.writeBuffer->pixelData;
                entry.writeBuffer->pixelData = nullptr;
            }
            entry.writeBuffer->width = entry.width;
            entry.writeBuffer->height = entry.height;
            size_t bufferSize = static_cast<size_t>(entry.width) * static_cast<size_t>(entry.height) * 4;
            if (bufferSize > 0 && bufferSize < 100 * 1024 * 1024) { entry.writeBuffer->pixelData = new unsigned char[bufferSize]; }
        }

        if (entry.writeBuffer->pixelData && entry.pixelData) {
            size_t copySize = static_cast<size_t>(entry.width) * static_cast<size_t>(entry.height) * 4;
            memcpy(entry.writeBuffer->pixelData, entry.pixelData, copySize);

            // Swap write and ready buffers under lock, then signal new frame available
            {
                std::lock_guard<std::mutex> lock(entry.swapMutex);
                entry.writeBuffer.swap(entry.readyBuffer);
            }

            // Signal that a new frame is available for the render thread
            entry.hasNewFrame.store(true, std::memory_order_release);
        }
    }

    SelectObject(hdcMem, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    if (hdcWindow) { ReleaseDC(targetHwnd, hdcWindow); }
    ReleaseDC(NULL, hdcScreen);

    // If Windows 10+ method failed and capture failed, show error texture
    if (!success && config.captureMethod != "BitBlt") {
        // Capture failed with Windows 10+ method - create dark blue error texture
        int errorWidth = 64;
        int errorHeight = 64;

        if (entry.width != errorWidth || entry.height != errorHeight) {
            if (entry.pixelData) {
                delete[] entry.pixelData;
                entry.pixelData = nullptr;
            }
            entry.width = errorWidth;
            entry.height = errorHeight;
            size_t bufferSize = static_cast<size_t>(errorWidth) * static_cast<size_t>(errorHeight) * 4;
            if (bufferSize > 0 && bufferSize < 1 * 1024 * 1024) {
                entry.pixelData = new unsigned char[bufferSize];
            }
        }

        for (int i = 0; i < errorWidth * errorHeight; i++) {
            entry.pixelData[i * 4 + 0] = 0;
            entry.pixelData[i * 4 + 1] = 32;
            entry.pixelData[i * 4 + 2] = 96;
            entry.pixelData[i * 4 + 3] = 255;
        }

        if (entry.writeBuffer->width != entry.width || entry.writeBuffer->height != entry.height) {
            if (entry.writeBuffer->pixelData) {
                delete[] entry.writeBuffer->pixelData;
                entry.writeBuffer->pixelData = nullptr;
            }
            entry.writeBuffer->width = entry.width;
            entry.writeBuffer->height = entry.height;
            size_t bufferSize = static_cast<size_t>(entry.width) * static_cast<size_t>(entry.height) * 4;
            if (bufferSize > 0 && bufferSize < 100 * 1024 * 1024) { entry.writeBuffer->pixelData = new unsigned char[bufferSize]; }
        }

        if (entry.writeBuffer->pixelData && entry.pixelData) {
            size_t copySize = static_cast<size_t>(entry.width) * static_cast<size_t>(entry.height) * 4;
            memcpy(entry.writeBuffer->pixelData, entry.pixelData, copySize);

            // Swap write and ready buffers under lock
            {
                std::lock_guard<std::mutex> lock(entry.swapMutex);
                entry.writeBuffer.swap(entry.readyBuffer);
            }

            // Signal that a new frame is available for the render thread
            entry.hasNewFrame.store(true, std::memory_order_release);
        }
    }

    return success;
}

const WindowOverlayConfig* FindWindowOverlayConfig(const std::string& overlayId) {
    // Note: Caller should hold g_configMutex
    const auto& overlays = g_config.windowOverlays;
    const size_t size = overlays.size();
    for (size_t i = 0; i < size; ++i) {
        if (overlays[i].name == overlayId) { return &overlays[i]; }
    }
    return nullptr;
}

const WindowOverlayConfig* FindWindowOverlayConfigIn(const std::string& overlayId, const Config& config) {
    const auto& overlays = config.windowOverlays;
    const size_t size = overlays.size();
    for (size_t i = 0; i < size; ++i) {
        if (overlays[i].name == overlayId) { return &overlays[i]; }
    }
    return nullptr;
}

void RemoveWindowOverlayFromCache(const std::string& overlayId) {
    std::lock_guard<std::mutex> cacheLock(g_windowOverlayCacheMutex);
    auto it = g_windowOverlayCache.find(overlayId);
    if (it != g_windowOverlayCache.end()) {
        if (it->second->glTextureId != 0) {
            glDeleteTextures(1, &it->second->glTextureId);
            it->second->glTextureId = 0;
        }
        g_windowOverlayCache.erase(it);
    }
}

bool StageWindowOverlayTestFrame(const WindowOverlayConfig& config, const std::vector<unsigned char>& rgbaPixels, int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }

    const size_t expectedByteCount = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    if (rgbaPixels.size() != expectedByteCount) {
        return false;
    }

    std::lock_guard<std::mutex> cacheLock(g_windowOverlayCacheMutex);
    auto& entrySlot = g_windowOverlayCache[config.name];
    if (!entrySlot) {
        entrySlot = std::make_unique<WindowOverlayCacheEntry>();
    }

    WindowOverlayCacheEntry& entry = *entrySlot;
    entry.windowTitle = config.windowTitle;
    entry.windowClass = config.windowClass;
    entry.executableName = config.executableName;
    entry.windowMatchPriority = config.windowMatchPriority;

    std::lock_guard<std::mutex> swapLock(entry.swapMutex);
    entry.readyBuffer = std::make_unique<WindowOverlayRenderData>();
    entry.readyBuffer->pixelData = new unsigned char[expectedByteCount];
    std::copy(rgbaPixels.begin(), rgbaPixels.end(), entry.readyBuffer->pixelData);
    entry.readyBuffer->width = width;
    entry.readyBuffer->height = height;
    entry.lastUploadedRenderData = nullptr;
    entry.hasNewFrame.store(true, std::memory_order_release);
    return true;
}

bool AcquireWindowOverlayPixelFrame(
    const WindowOverlayConfig& config, WindowOverlayPixelFrame& outFrame) {
    outFrame = {};
    std::lock_guard<std::mutex> cacheLock(g_windowOverlayCacheMutex);
    auto it = g_windowOverlayCache.find(config.name);
    if (it == g_windowOverlayCache.end() || !it->second) return false;
    WindowOverlayCacheEntry& entry = *it->second;
    if (entry.hasNewFrame.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> swapLock(entry.swapMutex);
        if (entry.hasNewFrame.load(std::memory_order_relaxed)) {
            entry.readyBuffer.swap(entry.backBuffer);
            entry.hasNewFrame.store(false, std::memory_order_release);
        }
    }
    WindowOverlayRenderData* renderData = entry.backBuffer.get();
    if (!renderData || !renderData->pixelData || renderData->width <= 0 ||
        renderData->height <= 0) {
        return false;
    }
    if (renderData != entry.lastPublishedRenderData || !entry.publishedPixels) {
        const size_t byteCount =
            static_cast<size_t>(renderData->width) *
            static_cast<size_t>(renderData->height) * 4;
        auto pixels = std::make_shared<std::vector<unsigned char>>(
            renderData->pixelData, renderData->pixelData + byteCount);
        entry.publishedPixels = std::move(pixels);
        entry.lastPublishedRenderData = renderData;
        ++entry.publishedGeneration;
    }
    outFrame.pixels = entry.publishedPixels;
    outFrame.width = renderData->width;
    outFrame.height = renderData->height;
    entry.glTextureWidth = renderData->width;
    entry.glTextureHeight = renderData->height;
    outFrame.generation = entry.publishedGeneration;
    return static_cast<bool>(outFrame.pixels);
}

void CleanupWindowOverlayCacheEntry(const std::string& overlayId) {
    // Don't actually erase the entry - this would cause crashes when capture thread is using it

    // First, get the config (without holding cache lock) - use snapshot for thread safety
    const WindowOverlayConfig* config = nullptr;
    WindowOverlayConfig configCopy;
    {
        auto cleanupSnap = GetConfigSnapshot();
        const WindowOverlayConfig* foundConfig = cleanupSnap ? FindWindowOverlayConfigIn(overlayId, *cleanupSnap) : nullptr;
        if (foundConfig) {
            configCopy = *foundConfig;
            config = &configCopy;
        }
    }

    // Now acquire cache lock and update
    std::lock_guard<std::mutex> cacheLock(g_windowOverlayCacheMutex);

    if (config) {
        LoadWindowOverlay_Internal(overlayId, *config);
    } else {
        auto it = g_windowOverlayCache.find(overlayId);
        if (it != g_windowOverlayCache.end()) {
            if (it->second->glTextureId != 0) {
                glDeleteTextures(1, &it->second->glTextureId);
                it->second->glTextureId = 0;
            }
            g_windowOverlayCache.erase(it);
        }
    }
}

void CleanupWindowOverlayCache() {
    std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);

    HGLRC currentContext = wglGetCurrentContext();
    if (currentContext) {
        for (auto& [id, entry] : g_windowOverlayCache) {
            if (entry && entry->glTextureId != 0) {
                try {
                    glDeleteTextures(1, &entry->glTextureId);
                    entry->glTextureId = 0;
                } catch (...) { Log("Exception cleaning up window overlay texture: " + id); }
            }
        }
    } else {
        Log("CleanupWindowOverlayCache: No valid GL context, skipping texture cleanup to avoid crashes");
    }

    g_windowOverlayCache.clear();
}

std::atomic<bool> g_windowOverlayInteractionActive{ false };
std::string g_focusedWindowOverlayName;
std::mutex g_focusedWindowOverlayMutex;

static void CalculateWindowOverlayDimensions(const WindowOverlayConfig& config, int& displayW, int& displayH) {
    std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
    auto it = g_windowOverlayCache.find(config.name);
    if (it != g_windowOverlayCache.end() && it->second) {
        int texWidth = it->second->glTextureWidth;
        int texHeight = it->second->glTextureHeight;
        if (texWidth > 0 && texHeight > 0) {
            auto cc = ResolveCrop(config.crop_top, config.crop_bottom, config.crop_left, config.crop_right,
                                  config.cropToWidth, config.cropToHeight, texWidth, texHeight);
            int croppedW = (std::max)(1, texWidth - cc.left - cc.right);
            int croppedH = (std::max)(1, texHeight - cc.top - cc.bottom);
            displayW = static_cast<int>(croppedW * config.scale);
            displayH = static_cast<int>(croppedH * config.scale);
            return;
        }
    }
    displayW = static_cast<int>(100 * config.scale);
    displayH = static_cast<int>(100 * config.scale);
}

std::string GetWindowOverlayAtPoint(int x, int y, int screenWidth, int screenHeight) {
    if (!g_windowOverlaysVisible.load(std::memory_order_acquire)) { return ""; }

    std::string currentModeId;
    {
        extern std::mutex g_modeIdMutex;
        extern std::string g_currentModeId;
        std::lock_guard<std::mutex> lock(g_modeIdMutex);
        currentModeId = g_currentModeId;
    }

    // Get the list of active window overlays for the current mode (use snapshot for thread safety)
    std::vector<std::pair<std::string, WindowOverlayConfig>> activeOverlays;
    {
        auto overlaySnap = GetConfigSnapshot();
        const ModeConfig* mode = overlaySnap ? GetModeFromSnapshotOrFallback(*overlaySnap, currentModeId) : nullptr;
        if (!mode) return "";

        for (auto it = mode->sources.rbegin(); it != mode->sources.rend(); ++it) {
            if (it->type != ModeSourceType::WindowOverlay) { continue; }
            const std::string& overlayId = it->id;
            const WindowOverlayConfig* config = FindWindowOverlayConfigIn(overlayId, *overlaySnap);
            if (config && config->enableInteraction) { activeOverlays.emplace_back(overlayId, *config); }
        }
    }

    ModeViewportInfo viewport = GetCurrentModeViewport();

    for (const auto& [overlayId, config] : activeOverlays) {
        int displayW, displayH;
        CalculateWindowOverlayDimensions(config, displayW, displayH);

        int finalScreenX, finalScreenY;

        bool isViewportRelative = IsViewportRelativeAnchor(config.relativeTo);

        if (isViewportRelative && viewport.valid) {
            GetRelativeCoordsForImageWithViewport(config.relativeTo, config.x, config.y, displayW, displayH, viewport.stretchX,
                                                  viewport.stretchY, viewport.stretchWidth, viewport.stretchHeight, screenWidth,
                                                  screenHeight, finalScreenX, finalScreenY);
        } else {
            GetRelativeCoordsForImage(config.relativeTo, config.x, config.y, displayW, displayH, screenWidth, screenHeight, finalScreenX,
                                      finalScreenY);
        }

        if (x >= finalScreenX && x < finalScreenX + displayW && y >= finalScreenY && y < finalScreenY + displayH) { return overlayId; }
    }

    return "";
}

HWND GetWindowOverlayHWND(const std::string& overlayName) {
    std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
    auto it = g_windowOverlayCache.find(overlayName);
    if (it != g_windowOverlayCache.end() && it->second) { return it->second->targetWindow.load(std::memory_order_relaxed); }
    return NULL;
}

bool TranslateToWindowOverlayCoords(const std::string& overlayName, int screenX, int screenY, int screenWidth, int screenHeight, int& outX,
                                    int& outY) {
    WindowOverlayConfig config;
    int texWidth = 0, texHeight = 0;

    // Get config and texture dimensions (use snapshot for thread safety)
    {
        auto translateSnap = GetConfigSnapshot();
        const WindowOverlayConfig* configPtr = translateSnap ? FindWindowOverlayConfigIn(overlayName, *translateSnap) : nullptr;
        if (!configPtr) return false;
        config = *configPtr;
    }

    {
        std::lock_guard<std::mutex> lock(g_windowOverlayCacheMutex);
        auto it = g_windowOverlayCache.find(overlayName);
        if (it == g_windowOverlayCache.end() || !it->second) return false;
        texWidth = it->second->glTextureWidth;
        texHeight = it->second->glTextureHeight;
    }

    if (texWidth <= 0 || texHeight <= 0) return false;

    auto cc = ResolveCrop(config.crop_top, config.crop_bottom, config.crop_left, config.crop_right,
                          config.cropToWidth, config.cropToHeight, texWidth, texHeight);
    int croppedWidth = (std::max)(1, texWidth - cc.left - cc.right);
    int croppedHeight = (std::max)(1, texHeight - cc.top - cc.bottom);
    int displayW = static_cast<int>(croppedWidth * config.scale);
    int displayH = static_cast<int>(croppedHeight * config.scale);

    if (displayW <= 0 || displayH <= 0) return false;

    int overlayScreenX, overlayScreenY;

    bool isViewportRelative = IsViewportRelativeAnchor(config.relativeTo);

    if (isViewportRelative) {
        ModeViewportInfo viewport = GetCurrentModeViewport();
        if (viewport.valid) {
            GetRelativeCoordsForImageWithViewport(config.relativeTo, config.x, config.y, displayW, displayH, viewport.stretchX,
                                                  viewport.stretchY, viewport.stretchWidth, viewport.stretchHeight, screenWidth,
                                                  screenHeight, overlayScreenX, overlayScreenY);
        } else {
            GetRelativeCoordsForImage(config.relativeTo, config.x, config.y, displayW, displayH, screenWidth, screenHeight, overlayScreenX,
                                      overlayScreenY);
        }
    } else {
        GetRelativeCoordsForImage(config.relativeTo, config.x, config.y, displayW, displayH, screenWidth, screenHeight, overlayScreenX,
                                  overlayScreenY);
    }

    float relX = static_cast<float>(screenX - overlayScreenX) / displayW;
    float relY = static_cast<float>(screenY - overlayScreenY) / displayH;

    relX = std::max(0.0f, std::min(1.0f, relX));
    relY = std::max(0.0f, std::min(1.0f, relY));

    outX = config.crop_left + static_cast<int>(relX * croppedWidth);
    outY = config.crop_top + static_cast<int>(relY * croppedHeight);

    return true;
}

void FocusWindowOverlay(const std::string& overlayName) {
    HWND targetHwnd = GetWindowOverlayHWND(overlayName);

    std::lock_guard<std::mutex> lock(g_focusedWindowOverlayMutex);
    g_focusedWindowOverlayName = overlayName;
    g_windowOverlayInteractionActive.store(true);
    Log("[WindowOverlay] Focused overlay for interaction: " + overlayName);

    if (targetHwnd && IsWindow(targetHwnd)) {
        PostMessage(targetHwnd, WM_SETFOCUS, 0, 0);
        PostMessage(targetHwnd, WM_ACTIVATE, WA_ACTIVE, 0);
    }
}

void UnfocusWindowOverlay() {
    std::string overlayToUnfocus;
    {
        std::lock_guard<std::mutex> lock(g_focusedWindowOverlayMutex);
        if (!g_focusedWindowOverlayName.empty()) {
            Log("[WindowOverlay] Unfocused overlay: " + g_focusedWindowOverlayName);
            overlayToUnfocus = g_focusedWindowOverlayName;
        }
        g_focusedWindowOverlayName = "";
        g_windowOverlayInteractionActive.store(false);
    }

    // Get HWND outside the lock to avoid deadlock (GetWindowOverlayHWND acquires g_windowOverlayCacheMutex)
    if (!overlayToUnfocus.empty()) {
        HWND targetHwnd = GetWindowOverlayHWND(overlayToUnfocus);
        if (targetHwnd && IsWindow(targetHwnd)) {
            PostMessage(targetHwnd, WM_KILLFOCUS, 0, 0);
            PostMessage(targetHwnd, WM_ACTIVATE, WA_INACTIVE, 0);
        }
    }
}

bool IsWindowOverlayFocused() { return g_windowOverlayInteractionActive.load(); }

std::string GetFocusedWindowOverlayName() {
    std::lock_guard<std::mutex> lock(g_focusedWindowOverlayMutex);
    return g_focusedWindowOverlayName;
}

bool ForwardMouseToWindowOverlay(UINT uMsg, int screenX, int screenY, WPARAM wParam, int screenWidth, int screenHeight) {
    if (!g_windowOverlayInteractionActive.load()) return false;

    std::string overlayName;
    {
        std::lock_guard<std::mutex> lock(g_focusedWindowOverlayMutex);
        overlayName = g_focusedWindowOverlayName;
    }

    if (overlayName.empty()) return false;

    HWND targetHwnd = GetWindowOverlayHWND(overlayName);
    if (!targetHwnd || !IsWindow(targetHwnd)) {
        UnfocusWindowOverlay();
        return false;
    }

    // Handle WM_MOUSEWHEEL and WM_MOUSEHWHEEL specially
    if (uMsg == WM_MOUSEWHEEL || uMsg == WM_MOUSEHWHEEL) {
        int windowX, windowY;
        if (!TranslateToWindowOverlayCoords(overlayName, screenX, screenY, screenWidth, screenHeight, windowX, windowY)) {
            RECT clientRect;
            if (GetClientRect(targetHwnd, &clientRect)) {
                windowX = (clientRect.right - clientRect.left) / 2;
                windowY = (clientRect.bottom - clientRect.top) / 2;
            } else {
                windowX = 0;
                windowY = 0;
            }
        }

        POINT targetScreenPos = { windowX, windowY };
        ClientToScreen(targetHwnd, &targetScreenPos);

        LPARAM wheelLParam = MAKELPARAM(targetScreenPos.x, targetScreenPos.y);

        SendMessage(targetHwnd, uMsg, wParam, wheelLParam);
        return true;
    }

    int windowX, windowY;
    if (!TranslateToWindowOverlayCoords(overlayName, screenX, screenY, screenWidth, screenHeight, windowX, windowY)) { return false; }

    LPARAM lParam = MAKELPARAM(windowX, windowY);

    PostMessage(targetHwnd, uMsg, wParam, lParam);

    return true;
}

bool ForwardKeyboardToWindowOverlay(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (!g_windowOverlayInteractionActive.load()) return false;

    std::string overlayName;
    {
        std::lock_guard<std::mutex> lock(g_focusedWindowOverlayMutex);
        overlayName = g_focusedWindowOverlayName;
    }

    if (overlayName.empty()) return false;

    if (uMsg == WM_KEYDOWN && wParam == VK_ESCAPE) {
        UnfocusWindowOverlay();
        return true;
    }

    HWND targetHwnd = GetWindowOverlayHWND(overlayName);
    if (!targetHwnd || !IsWindow(targetHwnd)) {
        UnfocusWindowOverlay();
        return false;
    }

    PostMessage(targetHwnd, uMsg, wParam, lParam);

    if (uMsg == WM_KEYDOWN) {
        WCHAR charCode = 0;

        if (wParam == VK_RETURN) {
            charCode = '\r';
        } else if (wParam == VK_TAB) {
            charCode = '\t';
        } else if (wParam == VK_BACK) {
            charCode = '\b';
        }

        if (charCode != 0) {
            PostMessage(targetHwnd, WM_CHAR, charCode, lParam);
        }
    }

    return true;
}

std::string GetExecutableNameFromWindow(HWND hwnd) {
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);

    if (processId == 0) { return ""; }

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!hProcess) { return ""; }

    char exePath[MAX_PATH];
    DWORD size = MAX_PATH;
    bool success = QueryFullProcessImageNameA(hProcess, 0, exePath, &size);
    CloseHandle(hProcess);

    if (success && size > 0) {
        const char* fileName = exePath;
        for (const char* p = exePath + size - 1; p >= exePath; --p) {
            if (*p == '\\' || *p == '/') {
                fileName = p + 1;
                break;
            }
        }
        return std::string(fileName);
    }

    return "";
}

BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam) {
    std::vector<WindowInfo>* windows = reinterpret_cast<std::vector<WindowInfo>*>(lParam);

    // Prevent selecting our own game window / helper windows as capture targets.
    // Since Toolscreen is injected, windows owned by this process are the game (and any of our helpers).
    HWND gameHwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
    if (gameHwnd && hwnd == gameHwnd) { return TRUE; }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0 && pid == GetCurrentProcessId()) { return TRUE; }

    // Skip invisible windows
    if (!IsWindowVisible(hwnd)) { return TRUE; }

    // Skip windows without titles (unless they have interesting classes)
    char windowTitle[256] = { 0 };
    GetWindowTextA(hwnd, windowTitle, sizeof(windowTitle) - 1);

    char windowClass[256] = { 0 };
    GetClassNameA(hwnd, windowClass, sizeof(windowClass) - 1);

    std::string title = windowTitle;
    std::string className = windowClass;
    std::string executableName = GetExecutableNameFromWindow(hwnd);

    static const std::vector<std::string> excludedExecutables = { "TextInputHost.exe", "RazerAppEngine.exe" };

    for (const auto& excluded : excludedExecutables) {
        if (executableName == excluded) { return TRUE; }
    }

    if (title.empty() && className.find("Chrome") == std::string::npos && className.find("Firefox") == std::string::npos &&
        className.find("Notepad") == std::string::npos) {
        return TRUE;
    }

    // Skip desktop and shell windows
    if (className == "Shell_TrayWnd" || className == "Progman" || className == "WorkerW" || className == "DV2ControlHost") { return TRUE; }

    WindowInfo info;
    info.title = title;
    info.className = className;
    info.executableName = executableName;
    info.hwnd = hwnd;

    windows->push_back(info);

    return TRUE;
}

// Get list of currently open windows
std::vector<WindowInfo> GetCurrentlyOpenWindows() {
    std::vector<WindowInfo> windows;
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&windows));

    std::sort(windows.begin(), windows.end(),
              [](const WindowInfo& a, const WindowInfo& b) { return a.GetDisplayName() < b.GetDisplayName(); });

    return windows;
}

bool IsWindowInfoValid(const WindowInfo& windowInfo) { return IsWindow(windowInfo.hwnd) && IsWindowVisible(windowInfo.hwnd); }

// Background capture thread function
void WindowCaptureThreadFunc() {
    _set_se_translator(SEHTranslator);

    try {
        Log("Window capture thread started");

        // Initialize window overlays on the background thread (avoids blocking render thread)
        // This is safe here because the window capture thread runs independently
        if (!g_windowOverlaysInitialized.load()) {
            Log("Initializing window overlays from capture thread");
            InitializeWindowOverlays();
            g_windowOverlaysInitialized.store(true);
        }

        auto lastWindowUpdateCheck = std::chrono::steady_clock::now();
        const auto windowUpdateInterval = std::chrono::seconds(5);

        auto lastWindowListUpdate = std::chrono::steady_clock::now();
        const auto windowListUpdateIntervalGuiOpen = std::chrono::milliseconds(500);
        const auto windowListUpdateIntervalGuiClosed = std::chrono::seconds(5);

        while (!g_stopWindowCaptureThread) {
            try {
                auto now = std::chrono::steady_clock::now();

                // Periodically update window handles in case windows were closed/reopened
                if (now - lastWindowUpdateCheck >= windowUpdateInterval) {
                    UpdateAllWindowOverlays();
                    lastWindowUpdateCheck = now;
                }

                // Periodically refresh the window list cache for the GUI (non-blocking window enumeration)
                const bool guiOpen = g_showGui.load(std::memory_order_relaxed);
                const auto listInterval = guiOpen ? windowListUpdateIntervalGuiOpen : windowListUpdateIntervalGuiClosed;
                if (now - lastWindowListUpdate >= listInterval) {
                    auto newWindowList = std::make_unique<std::vector<WindowInfo>>();
                    *newWindowList = GetCurrentlyOpenWindows();

                    {
                        std::lock_guard<std::mutex> lock(g_windowListCacheMutex);
                        auto oldList = g_windowListCache.exchange(newWindowList.release());
                        if (oldList) {
                            delete oldList;
                        }
                        g_lastWindowListUpdate = now;
                    }
                    lastWindowListUpdate = now;
                }

                // (We still keep the thread alive for quick re-enable.)
                if (!g_windowOverlaysVisible.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }

                // Process deferred overlay reloads (from GUI thread)
                {
                    std::vector<DeferredOverlayReload> reloadsToProcess;
                    {
                        std::lock_guard<std::mutex> lock(g_deferredOverlayReloadsMutex);
                        reloadsToProcess = std::move(g_deferredOverlayReloads);
                        g_deferredOverlayReloads.clear();
                    }

                    // Process reloads without holding the deferred queue lock
                    for (const auto& reload : reloadsToProcess) {
                        try {
                            LoadWindowOverlay(reload.overlayId, reload.config);
                            Log("Processed deferred reload for overlay: " + reload.overlayId);
                        } catch (const std::exception& e) {
                            Log("Error processing deferred reload for overlay '" + reload.overlayId + "': " + e.what());
                        }
                    }
                }

                std::vector<std::pair<std::string, WindowOverlayConfig>> overlaysToCapture;
                {
                    // Use snapshot for thread-safe config access + cache lock for cache access
                    auto captureSnap = GetConfigSnapshot();
                    std::lock_guard<std::mutex> cacheLock(g_windowOverlayCacheMutex);

                    const size_t cacheSize = g_windowOverlayCache.size();
                    overlaysToCapture.reserve(cacheSize);

                    for (const auto& [overlayId, entry] : g_windowOverlayCache) {
                        const WindowOverlayConfig* config = captureSnap ? FindWindowOverlayConfigIn(overlayId, *captureSnap) : nullptr;
                        if (config) { overlaysToCapture.emplace_back(overlayId, *config); }
                    }
                }

                if (overlaysToCapture.empty()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                // Locks released - now we can capture without blocking config/cache changes

                for (auto& [overlayId, config] : overlaysToCapture) {
                    if (g_stopWindowCaptureThread) { break; }

                    try {
                        // Get entry pointer with minimal lock duration
                        WindowOverlayCacheEntry* entry = nullptr;
                        {
                            std::lock_guard<std::mutex> cacheLock(g_windowOverlayCacheMutex);
                            auto it = g_windowOverlayCache.find(overlayId);
                            if (it != g_windowOverlayCache.end()) { entry = it->second.get(); }
                        }
                        // Lock released - now we can capture without blocking GUI thread

                        if (entry) {
                            // Capture without holding the cache mutex
                            // The entry's own captureMutex protects against concurrent modifications
                            // Note: Entry deletion is only done from GUI thread via RemoveWindowOverlayFromCache,
                            // and the capture thread checks for null before each capture
                            CaptureWindowContent(*entry, config);
                        }
                    } catch (const std::exception& e) {
                        Log("Error capturing window content for overlay '" + overlayId + "': " + e.what());
                    } catch (...) { Log("Unknown error capturing window content for overlay '" + overlayId + "'"); }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            } catch (const std::exception& e) { Log("Error in window capture thread: " + std::string(e.what())); } catch (...) {
                Log("Unknown error in window capture thread");
            }
        }
    } catch (const SE_Exception& e) {
        LogException("WindowCaptureThreadFunc (SEH)", e.getCode(), e.getInfo());
    } catch (const std::exception& e) { LogException("WindowCaptureThreadFunc", e); } catch (...) {
        Log("EXCEPTION in WindowCaptureThreadFunc: Unknown exception");
    }

    Log("Window capture thread stopped");
}

// Start background capture thread
void StartWindowCaptureThread() {
    if (!g_windowCaptureThread.joinable()) {
        g_stopWindowCaptureThread = false;
        g_windowCaptureThread = std::thread(WindowCaptureThreadFunc);
        Log("Started window capture background thread");
    }
}

// Stop background capture thread
void StopWindowCaptureThread() {
    if (g_windowCaptureThread.joinable()) {
        Log("Stopping window capture thread...");
        g_stopWindowCaptureThread = true;

        // Wait for thread to finish with timeout protection
        // Note: std::thread::join() will block until thread exits
        try {
            g_windowCaptureThread.join();
            Log("Window capture thread stopped cleanly");
        } catch (const std::system_error& e) { Log("Exception while joining window capture thread: " + std::string(e.what())); }
    }
}

// Get cached window list for GUI (non-blocking)
std::vector<WindowInfo> GetCachedWindowList() {
    std::lock_guard<std::mutex> lock(g_windowListCacheMutex);
    if (auto* cachedList = g_windowListCache.load()) {
        return *cachedList;
    }
    return std::vector<WindowInfo>();
}


