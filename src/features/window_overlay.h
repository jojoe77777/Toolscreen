#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "gui/gui.h"
#include "common/utils.h"
#include <atomic>
#include <cstdint>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

struct WindowOverlayConfig;
struct Color;

// Render data structure - contains only what the render thread needs (immutable after creation)
struct WindowOverlayRenderData {
    unsigned char* pixelData = nullptr;
    int width = 0;
    int height = 0;

    WindowOverlayRenderData() = default;
    ~WindowOverlayRenderData() {
        if (pixelData) {
            delete[] pixelData;
            pixelData = nullptr;
        }
    }

    WindowOverlayRenderData(const WindowOverlayRenderData&) = delete;
    WindowOverlayRenderData& operator=(const WindowOverlayRenderData&) = delete;
    WindowOverlayRenderData(WindowOverlayRenderData&& other) noexcept
        : pixelData(other.pixelData),
          width(other.width),
          height(other.height) {
        other.pixelData = nullptr;
        other.width = 0;
        other.height = 0;
    }
    WindowOverlayRenderData& operator=(WindowOverlayRenderData&& other) noexcept {
        if (this != &other) {
            if (pixelData) delete[] pixelData;
            pixelData = other.pixelData;
            width = other.width;
            height = other.height;
            other.pixelData = nullptr;
            other.width = 0;
            other.height = 0;
        }
        return *this;
    }
};

struct WindowOverlayPixelFrame {
    std::shared_ptr<const std::vector<unsigned char>> pixels;
    int width = 0;
    int height = 0;
    uint64_t generation = 0;
};

struct WindowOverlayCacheEntry {
    std::string windowTitle;
    std::string windowClass;
    std::string executableName;
    std::string windowMatchPriority = "title";
    std::atomic<HWND> targetWindow{ NULL };

    // Cached bitmap data (capture thread only)
    HBITMAP hBitmap = NULL;
    HDC hdcMem = NULL;
    unsigned char* pixelData = nullptr;
    int width = 0;
    int height = 0;

    // Triple-buffered render data for lock-free rendering
    // Capture thread writes to writeBuffer, then swaps with readyBuffer
    // Render thread swaps readyBuffer with backBuffer, then reads from backBuffer
    std::unique_ptr<WindowOverlayRenderData> writeBuffer;
    std::unique_ptr<WindowOverlayRenderData> readyBuffer;
    std::unique_ptr<WindowOverlayRenderData> backBuffer;           // Currently being read by render thread (safe from capture)
    std::atomic<bool> hasNewFrame{ false };                        // True when readyBuffer has new data for render thread
    std::mutex swapMutex; // Used for swapping buffers between threads

    // OpenGL texture caching (render thread only - no locking needed)
    unsigned int glTextureId = 0;
    int glTextureWidth = 0;
    int glTextureHeight = 0;
    WindowOverlayRenderData* lastUploadedRenderData = nullptr;
    WindowOverlayRenderData* lastPublishedRenderData = nullptr;
    std::shared_ptr<const std::vector<unsigned char>> publishedPixels;
    uint64_t publishedGeneration = 0;

    // Render-thread-only sampler state cache (avoids redundant glTexParameteri per frame)
    bool filterInitialized = false;
    bool lastPixelatedScaling = false;

    struct CachedRenderState {
        int crop_left = -1;
        int crop_right = -1;
        int crop_top = -1;
        int crop_bottom = -1;
        float scale = -1.0f;
        int x = 0;
        int y = 0;
        std::string relativeTo;
        int screenWidth = 0;
        int screenHeight = 0;

        int displayW = 0;
        int displayH = 0;
        int finalScreenX_win = 0;
        int finalScreenY_win = 0;
        float nx1 = 0, ny1 = 0, nx2 = 0, ny2 = 0;
        float tx1 = 0, ty1 = 0, tx2 = 0, ty2 = 0;

        bool isValid = false;
    } cachedRenderState;

    std::chrono::steady_clock::time_point lastCaptureTime;
    std::chrono::steady_clock::time_point lastRenderTime;
    std::atomic<int> fps{ 30 };

    std::chrono::steady_clock::time_point lastSearchTime;
    std::atomic<int> searchInterval{ 1000 };

    std::chrono::microseconds lastCaptureTimeUs{ 0 };
    std::chrono::microseconds lastUploadTimeUs{ 0 };

    // Thread safety
    std::mutex captureMutex;
    std::atomic<bool> needsUpdate{ true };

    WindowOverlayCacheEntry() {
        writeBuffer = std::make_unique<WindowOverlayRenderData>();
        readyBuffer = std::make_unique<WindowOverlayRenderData>();
        backBuffer = std::make_unique<WindowOverlayRenderData>();
    }
    ~WindowOverlayCacheEntry();

    // Delete copy and move constructors since std::mutex is not movable/copyable
    WindowOverlayCacheEntry(const WindowOverlayCacheEntry&) = delete;
    WindowOverlayCacheEntry& operator=(const WindowOverlayCacheEntry&) = delete;
    WindowOverlayCacheEntry(WindowOverlayCacheEntry&&) = delete;
    WindowOverlayCacheEntry& operator=(WindowOverlayCacheEntry&&) = delete;
};

// NOTE: InitializeWindowOverlays() is called from the background window capture thread
// to avoid blocking the render thread during expensive window searching
void InitializeWindowOverlays();
void LoadWindowOverlay(const std::string& overlayId, const WindowOverlayConfig& config);
void QueueOverlayReload(const std::string& overlayId, const WindowOverlayConfig& config); // Non-blocking deferred reload
void CleanupWindowOverlayCache();
void CleanupWindowOverlayCacheEntry(const std::string& overlayId);
void RemoveWindowOverlayFromCache(const std::string& overlayId);
bool StageWindowOverlayTestFrame(const WindowOverlayConfig& config, const std::vector<unsigned char>& rgbaPixels, int width, int height);
bool AcquireWindowOverlayPixelFrame(const WindowOverlayConfig& config, WindowOverlayPixelFrame& outFrame);
void UpdateWindowOverlay(const std::string& overlayId);
void UpdateWindowOverlayFPS(const std::string& overlayId, int newFPS);
void UpdateWindowOverlaySearchInterval(const std::string& overlayId, int newSearchInterval);
void UpdateAllWindowOverlays();
bool CaptureWindowContent(WindowOverlayCacheEntry& entry, const WindowOverlayConfig& config);
void RenderWindowOverlaysGL(const std::vector<std::string>& windowOverlayIds, int screenWidth, int screenHeight,
                            float opacityMultiplier = 1.0f, bool excludeOnlyOnMyScreen = false);
HWND FindWindowByTitleAndClass(const std::string& title, const std::string& className);
const WindowOverlayConfig* FindWindowOverlayConfig(const std::string& overlayId);
const WindowOverlayConfig* FindWindowOverlayConfigIn(const std::string& overlayId, const Config& config);

// Window enumeration callback for finding target windows
BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam);

struct WindowInfo {
    std::string title;
    std::string className;
    std::string executableName;
    HWND hwnd;

    std::string GetDisplayName() const {
        std::string display;
        if (!executableName.empty()) { display = "[" + executableName + "] "; }
        if (title.empty()) {
            display += "[No Title]";
        } else {
            display += title;
        }
        return display;
    }
};

// Function to get list of currently open windows for GUI dropdown
std::vector<WindowInfo> GetCurrentlyOpenWindows();

bool IsWindowInfoValid(const WindowInfo& windowInfo);

// Get cached window list (returns empty vector if not ready, avoids blocking GUI)
std::vector<WindowInfo> GetCachedWindowList();

std::string GetWindowOverlayProfilingInfo();

extern std::atomic<bool> g_windowOverlayInteractionActive;
extern std::string g_focusedWindowOverlayName;
extern std::mutex g_focusedWindowOverlayMutex;

std::string GetWindowOverlayAtPoint(int x, int y, int screenWidth, int screenHeight);

HWND GetWindowOverlayHWND(const std::string& overlayName);

bool TranslateToWindowOverlayCoords(const std::string& overlayName, int screenX, int screenY, int screenWidth, int screenHeight, int& outX,
                                    int& outY);

void FocusWindowOverlay(const std::string& overlayName);

void UnfocusWindowOverlay();

bool IsWindowOverlayFocused();

std::string GetFocusedWindowOverlayName();

bool ForwardMouseToWindowOverlay(UINT uMsg, int screenX, int screenY, WPARAM wParam, int screenWidth, int screenHeight);

bool ForwardKeyboardToWindowOverlay(UINT uMsg, WPARAM wParam, LPARAM lParam);

extern std::map<std::string, std::unique_ptr<WindowOverlayCacheEntry>> g_windowOverlayCache;
extern std::mutex g_windowOverlayCacheMutex;

extern std::atomic<std::vector<WindowInfo>*> g_windowListCache;
extern std::mutex g_windowListCacheMutex;
extern std::chrono::steady_clock::time_point g_lastWindowListUpdate;

// Background capture thread management
extern std::atomic<bool> g_stopWindowCaptureThread;
extern std::thread g_windowCaptureThread;
void WindowCaptureThreadFunc();
void StartWindowCaptureThread();
void StopWindowCaptureThread();


