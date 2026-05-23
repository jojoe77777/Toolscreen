#include "features/fake_cursor.h"
#include "features/cursor_trail.h"
#include "gui/gui.h"
#include "gui/imgui_cache.h"
#include "hooks/input_hook.h"
#include "runtime/logic_thread.h"
#include "render/mirror_thread.h"
#include "render/obs_thread.h"
#include "common/font_assets.h"
#include "common/profiler.h"
#include "render/render.h"
#include "platform/resource.h"
#include "common/i18n.h"
#include "hooks/hook_chain.h"
#include "common/utils.h"
#include "version.h"
#include "features/browser_overlay.h"
#include "features/virtual_camera.h"
#include "features/window_overlay.h"
#include "features/ninjabrain_client.h"

#include "MinHook.h"
#include <array>
#include <DbgHelp.h>
#include <Psapi.h>
#include <ShlObj.h>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <intrin.h>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <synchapi.h>
#include <thread>
#include <unordered_set>
#include <windowsx.h>

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Msimg32.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "libglew32.lib")
#pragma comment(lib, "DbgHelp.lib")

#define STB_IMAGE_IMPLEMENTATION
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "third_party/stb_image.h"

extern "C" NTSYSAPI PVOID NTAPI RtlPcToFileHeader(PVOID PcValue, PVOID* BaseOfImage);

Config g_config;
Config g_sharedConfig;
std::atomic<bool> g_configIsDirty{ false };

std::atomic<uint64_t> g_configSnapshotVersion{ 0 };

// CONFIG SNAPSHOT (RCU) - Lock-free immutable config for reader threads
// The mutable g_config is only touched by the GUI/main thread.
// Reader threads call GetConfigSnapshot() for a safe, lock-free snapshot.
static std::atomic<std::shared_ptr<const Config>> g_configSnapshot;

void PublishConfigSnapshot() {
    PublishConfigSnapshot(g_config);
}

void PublishConfigSnapshot(const Config& config) {
    Config sanitizedConfig = config;
    SanitizeConfigKeyRebindsForCannotTypeTriggers(sanitizedConfig);
    auto snapshot = std::make_shared<const Config>(std::move(sanitizedConfig));
    // Lock-free publish: atomic store of shared_ptr.
    g_configSnapshot.store(std::move(snapshot), std::memory_order_release);

    g_configSnapshotVersion.fetch_add(1, std::memory_order_release);
}

bool PublishConfigSnapshotIfUnchanged(const std::shared_ptr<const Config>& expectedSnapshot, const Config& config) {
    Config sanitizedConfig = config;
    SanitizeConfigKeyRebindsForCannotTypeTriggers(sanitizedConfig);
    auto snapshot = std::make_shared<const Config>(std::move(sanitizedConfig));
    auto expected = expectedSnapshot;
    if (!g_configSnapshot.compare_exchange_strong(expected, std::move(snapshot), std::memory_order_acq_rel, std::memory_order_acquire)) {
        return false;
    }

    g_configSnapshotVersion.fetch_add(1, std::memory_order_release);
    return true;
}

std::shared_ptr<const Config> GetConfigSnapshot() {
    // Lock-free read: atomic load of shared_ptr.
    return g_configSnapshot.load(std::memory_order_acquire);
}

std::string GetPublishedCurrentModeId() {
    const int index = g_currentModeIdIndex.load(std::memory_order_acquire);
    return g_modeIdBuffers[index];
}

// HOTKEY SECONDARY MODE STATE - Thread-safe runtime state separated from Config
static std::vector<std::string> g_hotkeySecondaryModes;
std::mutex g_hotkeySecondaryModesMutex;

std::string GetHotkeySecondaryMode(size_t hotkeyIndex) {
    std::lock_guard<std::mutex> lock(g_hotkeySecondaryModesMutex);
    if (hotkeyIndex < g_hotkeySecondaryModes.size()) { return g_hotkeySecondaryModes[hotkeyIndex]; }
    return "";
}

void SetHotkeySecondaryMode(size_t hotkeyIndex, const std::string& mode) {
    std::lock_guard<std::mutex> lock(g_hotkeySecondaryModesMutex);
    if (hotkeyIndex >= g_hotkeySecondaryModes.size()) { g_hotkeySecondaryModes.resize(hotkeyIndex + 1); }
    g_hotkeySecondaryModes[hotkeyIndex] = mode;
}

void ResetAllHotkeySecondaryModes() {
    std::lock_guard<std::mutex> lock(g_hotkeySecondaryModesMutex);
    g_hotkeySecondaryModes.resize(g_config.hotkeys.size());
    for (size_t i = 0; i < g_config.hotkeys.size(); ++i) { g_hotkeySecondaryModes[i] = g_config.hotkeys[i].secondaryMode; }
}

void ResetAllHotkeySecondaryModes(const Config& config) {
    std::lock_guard<std::mutex> lock(g_hotkeySecondaryModesMutex);
    g_hotkeySecondaryModes.resize(config.hotkeys.size());
    for (size_t i = 0; i < config.hotkeys.size(); ++i) { g_hotkeySecondaryModes[i] = config.hotkeys[i].secondaryMode; }
}

void ResizeHotkeySecondaryModes(size_t count) {
    std::lock_guard<std::mutex> lock(g_hotkeySecondaryModesMutex);
    g_hotkeySecondaryModes.resize(count);
}

TempSensitivityOverride g_tempSensitivityOverride;
std::mutex g_tempSensitivityMutex;

void ClearTempSensitivityOverride() {
    std::lock_guard<std::mutex> lock(g_tempSensitivityMutex);
    g_tempSensitivityOverride.active = false;
    g_tempSensitivityOverride.sensitivityX = 1.0f;
    g_tempSensitivityOverride.sensitivityY = 1.0f;
    g_tempSensitivityOverride.activeSensHotkeyIndex = -1;
}

std::atomic<bool> g_cursorsNeedReload{ false };
std::atomic<bool> g_showGui{ false };
std::atomic<bool> g_imageOverlaysVisible{ true };
std::atomic<bool> g_windowOverlaysVisible{ true };
std::atomic<bool> g_ninjabrainOverlayVisible{ true };
std::atomic<bool> g_browserOverlaysVisible{ true };
std::string g_currentlyEditingMirror;
std::atomic<HWND> g_minecraftHwnd{ NULL };
std::wstring g_toolscreenPath;
std::string g_currentModeId = "";
std::mutex g_modeIdMutex;
// Lock-free mode ID access (double-buffered) - input handlers read from these without locking
std::string g_modeIdBuffers[2] = { "", "" };
std::atomic<int> g_currentModeIdIndex{ 0 };
std::atomic<bool> g_screenshotRequested{ false };
std::atomic<bool> g_pendingImageLoad{ false };
std::string g_configLoadError;
std::mutex g_configErrorMutex;
std::wstring g_modeFilePath;
std::atomic<bool> g_configLoadFailed{ false };
std::atomic<bool> g_configLoaded{ false };
std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_hotkeyTimestamps;
std::atomic<bool> g_guiNeedsRecenter{ true };
std::atomic<bool> g_wasCursorVisible{ true };
std::atomic<bool> g_forceVisibleCursorWhileGuiOpen{ false };
constexpr int kDeferredGuiGlfwCursorModeNone = 0;
std::atomic<int> g_deferredGuiGlfwCursorMode{ kDeferredGuiGlfwCursorModeNone };
std::atomic<void*> g_lastGlfwCursorWindow{ nullptr };
// Lock-free GUI toggle debounce timestamp
std::atomic<int64_t> g_lastGuiToggleTimeMs{ 0 };

enum CapturingState { NONE = 0, DISABLED = 1, NORMAL = 2 };
std::atomic<CapturingState> g_capturingMousePos{ CapturingState::NONE };
std::atomic<std::pair<int, int>> g_nextMouseXY{ std::make_pair(-1, -1) };

std::unordered_set<DWORD> g_hotkeyMainKeys;
std::mutex g_hotkeyMainKeysMutex;

std::mutex g_hotkeyTimestampsMutex;

// Track trigger-on-release hotkeys that are currently pressed
std::set<std::string> g_triggerOnReleasePending;
// Track which pending trigger-on-release hotkeys have been invalidated
std::set<std::string> g_triggerOnReleaseInvalidated;
std::mutex g_triggerOnReleaseMutex;

std::atomic<bool> g_imageDragMode{ false };
std::string g_draggedImageName = "";
std::mutex g_imageDragMutex;

std::atomic<bool> g_windowOverlayDragMode{ false };
std::atomic<bool> g_browserOverlayDragMode{ false };

std::ofstream logFile;
std::mutex g_logFileMutex;
static LogSession g_logSession;
HMODULE g_hModule = NULL;

GameVersion g_gameVersion;

bool g_glewLoaded = false;
WNDPROC g_originalWndProc = NULL;
std::atomic<HWND> g_subclassedHwnd{ NULL };
std::atomic<bool> g_hwndChanged{ false };
std::atomic<bool> g_isShuttingDown{ false };
std::atomic<bool> g_allImagesLoaded{ false };
std::atomic<bool> g_isTransitioningMode{ false };
std::atomic<bool> g_skipViewportAnimation{ false };
std::atomic<int> g_wmMouseMoveCount{ 0 };

ModeTransitionAnimation g_modeTransition;
std::mutex g_modeTransitionMutex;
// Lock-free snapshot for viewport hook
ViewportTransitionSnapshot g_viewportTransitionSnapshots[2];
std::atomic<int> g_viewportTransitionSnapshotIndex{ 0 };

PendingModeSwitch g_pendingModeSwitch;
std::mutex g_pendingModeSwitchMutex;

std::atomic<double> g_lastFrameTimeMs{ 0.0 };
std::atomic<double> g_originalFrameTimeMs{ 0.0 };

std::chrono::high_resolution_clock::time_point g_lastFrameEndTime = std::chrono::high_resolution_clock::now();
std::mutex g_fpsLimitMutex;
HANDLE g_highResTimer = NULL;
int g_originalWindowsMouseSpeed = 0;                      // Original Windows mouse speed to restore on exit
std::atomic<bool> g_windowsMouseSpeedApplied{ false };
FILTERKEYS g_originalFilterKeys = { sizeof(FILTERKEYS) };
std::atomic<bool> g_filterKeysApplied{ false };
std::atomic<bool> g_originalFilterKeysCaptured{ false };

std::string g_lastFrameModeId = "";
std::mutex g_lastFrameModeIdMutex;
// Lock-free last frame mode ID for viewport hook
std::string g_lastFrameModeIdBuffers[2] = { "", "" };
std::atomic<int> g_lastFrameModeIdIndex{ 0 };
std::string g_gameStateBuffers[2] = { "title", "title" };
std::atomic<int> g_currentGameStateIndex{ 0 };
const ModeConfig* g_currentMode = nullptr;

std::atomic<bool> g_gameWindowActive{ false };

std::thread g_monitorThread;
std::thread g_imageMonitorThread;
static std::thread g_hookCompatThread;
static std::atomic<bool> g_stopHookCompat{ false };
HANDLE g_resizeThread = NULL;
std::atomic<bool> g_stopMonitoring{ false };
std::atomic<bool> g_stopImageMonitoring{ false };
std::wstring g_stateFilePath;
std::atomic<bool> g_isStateOutputAvailable{ false };

std::vector<DecodedImageData> g_decodedImagesQueue;
std::mutex g_decodedImagesMutex;

std::atomic<GLuint> g_cachedGameTextureId{ UINT_MAX };
std::atomic<GLuint> g_lastTrackedGameFramebufferTextureId{ UINT_MAX };
std::atomic<GLuint> g_lastTrackedGameTextureBindId{ UINT_MAX };
std::atomic<DWORD> g_lastSwapBuffersThreadId{ 0 };

std::atomic<HCURSOR> g_specialCursorHandle{ NULL };

std::atomic<bool> g_graphicsHookDetected{ false };
std::atomic<HMODULE> g_graphicsHookModule{ NULL };
std::chrono::steady_clock::time_point g_lastGraphicsHookCheck = std::chrono::steady_clock::now();
extern const int GRAPHICS_HOOK_CHECK_INTERVAL_MS = 2000;

std::atomic<bool> g_obsCaptureReady{ false };
static constexpr int SAME_THREAD_OBS_CAPTURE_BUFFER_COUNT = 2;
static GLuint g_sameThreadObsCaptureFBOs[SAME_THREAD_OBS_CAPTURE_BUFFER_COUNT] = {};
static GLuint g_sameThreadObsCaptureTextures[SAME_THREAD_OBS_CAPTURE_BUFFER_COUNT] = {};
static int g_sameThreadObsCaptureWidth = 0;
static int g_sameThreadObsCaptureHeight = 0;
static int g_sameThreadObsCapturePublishedIndex = -1;
static int g_sameThreadObsCaptureWriteIndex = 0;

void LoadConfig();
void SaveConfig();
void RenderSettingsGUI();
void AttemptAggressiveGlViewportHook();
void ApplyWindowsMouseSpeed();
void ApplyKeyRepeatSettings();

void InvalidateTrackedGameTextureId(bool clearSwapThread, bool clearCachedTexture) {
    if (clearCachedTexture) {
        g_cachedGameTextureId.store(UINT_MAX, std::memory_order_release);
    }
    g_lastTrackedGameFramebufferTextureId.store(UINT_MAX, std::memory_order_release);
    g_lastTrackedGameTextureBindId.store(UINT_MAX, std::memory_order_release);
    if (clearSwapThread) {
        g_lastSwapBuffersThreadId.store(0, std::memory_order_release);
    }
}

static void SyncVirtualCameraRuntimeState(bool enabled) {
    static constexpr auto kVirtualCameraRetryInterval = std::chrono::milliseconds(100);
    static auto s_lastStartAttempt = std::chrono::steady_clock::time_point{};
    static uint32_t s_lastAttemptWidth = 0;
    static uint32_t s_lastAttemptHeight = 0;

    if (!enabled) {
        if (IsVirtualCameraActive()) { StopVirtualCamera(); }
        s_lastStartAttempt = std::chrono::steady_clock::time_point{};
        s_lastAttemptWidth = 0;
        s_lastAttemptHeight = 0;
        return;
    }

    if (!IsVirtualCameraDriverInstalled()) { return; }

    uint32_t width = 0;
    uint32_t height = 0;
    if (!GetPreferredVirtualCameraResolution(width, height)) { return; }

    // Flush any debounced resize from OnGameWindowResized
    FlushPendingVirtualCameraResize();

    if (IsVirtualCameraActive()) {
        // If already active but at a different preferred resolution, resize in-place
        uint32_t activeWidth = 0, activeHeight = 0;
        if (GetVirtualCameraResolution(activeWidth, activeHeight) &&
            (activeWidth != width || activeHeight != height)) {
            EnsureVirtualCameraSize(width, height);
        }
        s_lastStartAttempt = std::chrono::steady_clock::time_point{};
        return;
    }

    if (IsVirtualCameraInUseByOBS()) { return; }

    const auto now = std::chrono::steady_clock::now();
    const bool dimensionsChanged = width != s_lastAttemptWidth || height != s_lastAttemptHeight;
    if (!dimensionsChanged && s_lastStartAttempt.time_since_epoch().count() != 0 &&
        (now - s_lastStartAttempt) < kVirtualCameraRetryInterval) {
        return;
    }

    s_lastStartAttempt = now;
    s_lastAttemptWidth = width;
    s_lastAttemptHeight = height;
    StartVirtualCamera(width, height);
}

void CaptureBackbufferForObs(int width, int height) {
    PROFILE_SCOPE_CAT("Capture Backbuffer for OBS", "OBS");

    if (width <= 0 || height <= 0) {
        ClearObsOverride();
        g_sameThreadObsCapturePublishedIndex = -1;
        g_sameThreadObsCaptureWriteIndex = 0;
        return;
    }

    const bool needsResize = width != g_sameThreadObsCaptureWidth || height != g_sameThreadObsCaptureHeight;
    const bool hasAllTargets = g_sameThreadObsCaptureFBOs[0] != 0 && g_sameThreadObsCaptureFBOs[1] != 0 &&
                               g_sameThreadObsCaptureTextures[0] != 0 && g_sameThreadObsCaptureTextures[1] != 0;
    if (needsResize || !hasAllTargets) {
        ClearObsOverride();
        for (int i = 0; i < SAME_THREAD_OBS_CAPTURE_BUFFER_COUNT; ++i) {
            if (g_sameThreadObsCaptureFBOs[i] == 0) { glGenFramebuffers(1, &g_sameThreadObsCaptureFBOs[i]); }
            if (g_sameThreadObsCaptureTextures[i] != 0) {
                glDeleteTextures(1, &g_sameThreadObsCaptureTextures[i]);
                g_sameThreadObsCaptureTextures[i] = 0;
            }

            glGenTextures(1, &g_sameThreadObsCaptureTextures[i]);
            BindTextureDirect(GL_TEXTURE_2D, g_sameThreadObsCaptureTextures[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            glBindFramebuffer(GL_FRAMEBUFFER, g_sameThreadObsCaptureFBOs[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sameThreadObsCaptureTextures[i], 0);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        BindTextureDirect(GL_TEXTURE_2D, 0);

        g_sameThreadObsCaptureWidth = width;
        g_sameThreadObsCaptureHeight = height;
        g_sameThreadObsCapturePublishedIndex = -1;
        g_sameThreadObsCaptureWriteIndex = 0;
    }

    if (!ShouldUpdateObsTextureNow()) {
        return;
    }

    const int captureIndex = g_sameThreadObsCaptureWriteIndex;
    if (captureIndex < 0 || captureIndex >= SAME_THREAD_OBS_CAPTURE_BUFFER_COUNT || g_sameThreadObsCaptureFBOs[captureIndex] == 0 ||
        g_sameThreadObsCaptureTextures[captureIndex] == 0) {
        return;
    }

    GLint prevReadFBO = 0;
    GLint prevDrawFBO = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevReadFBO);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDrawFBO);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_sameThreadObsCaptureFBOs[captureIndex]);
    BlitFramebufferDirect(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prevReadFBO);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prevDrawFBO);

    SetObsOverrideTexture(g_sameThreadObsCaptureTextures[captureIndex], width, height);
    g_sameThreadObsCapturePublishedIndex = captureIndex;
    g_sameThreadObsCaptureWriteIndex = (captureIndex + 1) % SAME_THREAD_OBS_CAPTURE_BUFFER_COUNT;
}

GLuint GetObsCaptureTexture() {
    if (g_sameThreadObsCapturePublishedIndex < 0 || g_sameThreadObsCapturePublishedIndex >= SAME_THREAD_OBS_CAPTURE_BUFFER_COUNT) {
        return 0;
    }
    return g_sameThreadObsCaptureTextures[g_sameThreadObsCapturePublishedIndex];
}

int GetObsCaptureWidth() { return g_sameThreadObsCapturePublishedIndex >= 0 ? g_sameThreadObsCaptureWidth : 0; }

int GetObsCaptureHeight() { return g_sameThreadObsCapturePublishedIndex >= 0 ? g_sameThreadObsCaptureHeight : 0; }


bool SubclassGameWindow(HWND hwnd) {
    if (!hwnd) return false;

    if (g_isShuttingDown.load()) return false;

    HWND currentSubclassed = g_subclassedHwnd.load();
    if (currentSubclassed == hwnd && g_originalWndProc != NULL) {
        return true;
    }

    if (currentSubclassed != NULL && currentSubclassed != hwnd) {
        Log("Window handle changed from " + std::to_string(reinterpret_cast<uintptr_t>(currentSubclassed)) + " to " +
            std::to_string(reinterpret_cast<uintptr_t>(hwnd)) + " (likely fullscreen toggle)");
        g_originalWndProc = NULL;

        g_minecraftHwnd.store(hwnd);
        InvalidateTrackedGameTextureId(false);
        g_hwndChanged.store(true);
    }

    WNDPROC oldProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassedWndProc);
    if (oldProc) {
        g_originalWndProc = oldProc;
        g_subclassedHwnd.store(hwnd);
        if (IsWindowInForegroundTree(hwnd)) {
            g_gameWindowActive.store(true, std::memory_order_release);
            ApplyWindowsMouseSpeed();
            ApplyKeyRepeatSettings();
            ApplyConfineCursorToGameWindow();
        }
        Log("Successfully subclassed window: " + std::to_string(reinterpret_cast<uintptr_t>(hwnd)));
        return true;
    } else {
        Log("ERROR: Failed to subclass window: " + std::to_string(reinterpret_cast<uintptr_t>(hwnd)));
        return false;
    }
}

template <typename T> bool CreateHookOrDie(LPVOID pTarget, LPVOID pDetour, T** ppOriginal, const char* hookName) {
    if (pTarget == NULL) {
        std::string warnMsg = std::string("WARNING: ") + hookName + " function not found (NULL pointer)";
        Log(warnMsg);
        return false;
    }
    if (MH_CreateHook(pTarget, pDetour, reinterpret_cast<void**>(ppOriginal)) != MH_OK) {
        std::string errorMsg = std::string("ERROR: ") + hookName + " hook failed!";
        Log(errorMsg);
        return false;
    }
    LogCategory("init", "Created hook for " + std::string(hookName));
    return true;
}

// REQUIRES: g_configMutex and g_hotkeyMainKeysMutex must already be held by caller
void RebuildHotkeyMainKeys_Internal() {
    g_hotkeyMainKeys.clear();

    auto isModifier = [](DWORD key) {
        return key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL || key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
               key == VK_MENU || key == VK_LMENU || key == VK_RMENU;
    };

    auto addMainKey = [&](const std::vector<DWORD>& keys) {
        if (keys.empty()) return;
        DWORD mainKey = keys.back();
        g_hotkeyMainKeys.insert(mainKey);

        // For modifier keys, also add the generic version since Windows sends
        if (mainKey == VK_LCONTROL || mainKey == VK_RCONTROL) {
            g_hotkeyMainKeys.insert(VK_CONTROL);
        } else if (mainKey == VK_CONTROL) {
            g_hotkeyMainKeys.insert(VK_LCONTROL);
            g_hotkeyMainKeys.insert(VK_RCONTROL);
        } else if (mainKey == VK_LSHIFT || mainKey == VK_RSHIFT) {
            g_hotkeyMainKeys.insert(VK_SHIFT);
        } else if (mainKey == VK_SHIFT) {
            g_hotkeyMainKeys.insert(VK_LSHIFT);
            g_hotkeyMainKeys.insert(VK_RSHIFT);
        } else if (mainKey == VK_LMENU || mainKey == VK_RMENU) {
            g_hotkeyMainKeys.insert(VK_MENU);
        } else if (mainKey == VK_MENU) {
            g_hotkeyMainKeys.insert(VK_LMENU);
            g_hotkeyMainKeys.insert(VK_RMENU);
        }
    };

    for (const auto& hotkey : g_config.hotkeys) {
        addMainKey(hotkey.keys);

        for (const auto& alt : hotkey.altSecondaryModes) { addMainKey(alt.keys); }
    }

    for (const auto& sensHotkey : g_config.sensitivityHotkeys) { addMainKey(sensHotkey.keys); }

    addMainKey(g_config.guiHotkey);

    addMainKey(g_config.borderlessHotkey);

    addMainKey(g_config.imageOverlaysHotkey);

    addMainKey(g_config.windowOverlaysHotkey);

    addMainKey(g_config.ninjabrainOverlayHotkey);

    addMainKey(g_config.keyRebinds.toggleHotkey);

    g_hotkeyMainKeys.insert(VK_ESCAPE);

    if (g_config.keyRebinds.enabled) {
        for (const auto& rebind : g_config.keyRebinds.rebinds) {
            if (rebind.enabled && rebind.fromKey != 0) {
                g_hotkeyMainKeys.insert(rebind.fromKey);

                // Windows may deliver VK_SHIFT in wParam (and vice-versa).
                if (rebind.fromKey == VK_LCONTROL || rebind.fromKey == VK_RCONTROL) {
                    g_hotkeyMainKeys.insert(VK_CONTROL);
                } else if (rebind.fromKey == VK_CONTROL) {
                    g_hotkeyMainKeys.insert(VK_LCONTROL);
                    g_hotkeyMainKeys.insert(VK_RCONTROL);
                } else if (rebind.fromKey == VK_LSHIFT || rebind.fromKey == VK_RSHIFT) {
                    g_hotkeyMainKeys.insert(VK_SHIFT);
                } else if (rebind.fromKey == VK_SHIFT) {
                    g_hotkeyMainKeys.insert(VK_LSHIFT);
                    g_hotkeyMainKeys.insert(VK_RSHIFT);
                } else if (rebind.fromKey == VK_LMENU || rebind.fromKey == VK_RMENU) {
                    g_hotkeyMainKeys.insert(VK_MENU);
                } else if (rebind.fromKey == VK_MENU) {
                    g_hotkeyMainKeys.insert(VK_LMENU);
                    g_hotkeyMainKeys.insert(VK_RMENU);
                }
            }
        }
    }
}

// This version acquires both required locks - use when you don't already hold them
void RebuildHotkeyMainKeys() {
    std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
    RebuildHotkeyMainKeys_Internal();
}

// Save the original Windows mouse speed setting
void SaveOriginalWindowsMouseSpeed() {
    int currentSpeed = 0;
    if (SystemParametersInfo(SPI_GETMOUSESPEED, 0, &currentSpeed, 0)) {
        g_originalWindowsMouseSpeed = currentSpeed;
        LogCategory("init", "Saved original Windows mouse speed: " + std::to_string(currentSpeed));
    } else {
        Log("WARNING: Failed to get current Windows mouse speed");
        g_originalWindowsMouseSpeed = 10;
    }
}

void RestoreWindowsMouseSpeed();
void RestoreKeyRepeatSettings();

static bool ShouldOwnGlobalInputState() {
    const HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
    return IsWindowInForegroundTree(hwnd);
}

// Apply the configured Windows mouse speed (if enabled)
void ApplyWindowsMouseSpeed() {
    if (!ShouldOwnGlobalInputState()) {
        RestoreWindowsMouseSpeed();
        return;
    }

    int targetSpeed = g_config.windowsMouseSpeed;

    if (targetSpeed == 0) {
        if (g_windowsMouseSpeedApplied.load()) {
            if (SystemParametersInfo(SPI_SETMOUSESPEED, 0, reinterpret_cast<void*>(static_cast<intptr_t>(g_originalWindowsMouseSpeed)),
                                     0)) {
                Log("Restored Windows mouse speed to: " + std::to_string(g_originalWindowsMouseSpeed));
            }
            g_windowsMouseSpeedApplied.store(false);
        }
        return;
    }

    if (targetSpeed < 1) targetSpeed = 1;
    if (targetSpeed > 20) targetSpeed = 20;

    if (SystemParametersInfo(SPI_SETMOUSESPEED, 0, reinterpret_cast<void*>(static_cast<intptr_t>(targetSpeed)), 0)) {
        g_windowsMouseSpeedApplied.store(true);
        Log("Applied Windows mouse speed: " + std::to_string(targetSpeed));
    } else {
        Log("WARNING: Failed to set Windows mouse speed to: " + std::to_string(targetSpeed));
    }
}

// Restore the original Windows mouse speed on shutdown
void RestoreWindowsMouseSpeed() {
    if (g_windowsMouseSpeedApplied.load()) {
        if (SystemParametersInfo(SPI_SETMOUSESPEED, 0, reinterpret_cast<void*>(static_cast<intptr_t>(g_originalWindowsMouseSpeed)), 0)) {
            Log("Restored Windows mouse speed to: " + std::to_string(g_originalWindowsMouseSpeed));
        } else {
            Log("WARNING: Failed to restore Windows mouse speed");
        }
        g_windowsMouseSpeedApplied.store(false);
    }
}

void SaveOriginalKeyRepeatSettings() {
    g_originalFilterKeys.cbSize = sizeof(FILTERKEYS);
    if (SystemParametersInfo(SPI_GETFILTERKEYS, sizeof(FILTERKEYS), &g_originalFilterKeys, 0)) {
        g_originalFilterKeysCaptured.store(true);
        LogCategory("init", "Saved original FILTERKEYS: flags=0x" + std::to_string(g_originalFilterKeys.dwFlags) +
                                ", iDelayMSec=" + std::to_string(g_originalFilterKeys.iDelayMSec) +
                                ", iRepeatMSec=" + std::to_string(g_originalFilterKeys.iRepeatMSec));
    } else {
        Log("WARNING: Failed to get current FILTERKEYS settings");
        g_originalFilterKeys.dwFlags = 0;
        g_originalFilterKeys.iDelayMSec = 0;
        g_originalFilterKeys.iRepeatMSec = 0;
        g_originalFilterKeysCaptured.store(false);
    }
}

static int GetFallbackKeyboardDelayMs() {
    UINT keyboardDelay = 0;
    if (SystemParametersInfo(SPI_GETKEYBOARDDELAY, 0, &keyboardDelay, 0)) {
        keyboardDelay = (std::min)(keyboardDelay, 3u);
        return static_cast<int>((keyboardDelay + 1u) * 250u);
    }
    return 250;
}

static int GetFallbackKeyboardRepeatMs() {
    UINT keyboardSpeed = 31;
    if (SystemParametersInfo(SPI_GETKEYBOARDSPEED, 0, &keyboardSpeed, 0)) {
        keyboardSpeed = (std::min)(keyboardSpeed, 31u);
        const double repeatsPerSecond = 2.5 + (27.5 * (static_cast<double>(keyboardSpeed) / 31.0));
        if (repeatsPerSecond > 0.0) {
            return (std::max)(1, static_cast<int>(std::lround(1000.0 / repeatsPerSecond)));
        }
    }
    return 33;
}

static int ClampConfiguredKeyRepeatStartDelayValue(int value, bool useSystemKeyRepeat) {
    (void)useSystemKeyRepeat;
    if (value < 0) {
        return -1;
    }
    if (value < 100) {
        return 100;
    }

    value = (std::min)(value, 300);
    if (useSystemKeyRepeat) {
        return value;
    }
    return 100 + (((value - 100) + 2) / 5) * 5;
}

static int ClampConfiguredKeyRepeatDelayValue(int value, bool useSystemKeyRepeat) {
    if (value < 0) {
        return -1;
    }
    value = (std::max)(value, 1);
    return (std::min)(value, useSystemKeyRepeat ? 300 : 50);
}

static int GetSystemDefaultKeyRepeatStartDelayMs() {
    if (g_originalFilterKeysCaptured.load(std::memory_order_acquire) && g_originalFilterKeys.iDelayMSec > 0) {
        return g_originalFilterKeys.iDelayMSec;
    }
    return GetFallbackKeyboardDelayMs();
}

static int GetSystemDefaultKeyRepeatDelayMs() {
    if (g_originalFilterKeysCaptured.load(std::memory_order_acquire) && g_originalFilterKeys.iRepeatMSec > 0) {
        return g_originalFilterKeys.iRepeatMSec;
    }
    return GetFallbackKeyboardRepeatMs();
}

bool GetEffectiveKeyRepeatTimings(int& outStartDelayMs, int& outRepeatDelayMs) {
    constexpr bool useSystemKeyRepeat = true;
    int configuredStartDelay = ClampConfiguredKeyRepeatStartDelayValue(g_config.keyRepeatStartDelay, useSystemKeyRepeat);
    int configuredRepeatDelay = ClampConfiguredKeyRepeatDelayValue(g_config.keyRepeatDelay, useSystemKeyRepeat);
    outStartDelayMs = (configuredStartDelay >= 0)
                          ? configuredStartDelay
                          : (useSystemKeyRepeat ? GetSystemDefaultKeyRepeatStartDelayMs()
                                                : ConfigDefaults::CONFIG_KEY_REPEAT_AUTO_START_DELAY_MS);
    outRepeatDelayMs = (configuredRepeatDelay >= 0)
                           ? configuredRepeatDelay
                           : (useSystemKeyRepeat ? GetSystemDefaultKeyRepeatDelayMs() : ConfigDefaults::CONFIG_KEY_REPEAT_AUTO_DELAY_MS);
    outStartDelayMs = (std::max)(outStartDelayMs, 1);
    outRepeatDelayMs = (std::max)(outRepeatDelayMs, 1);
    return true;
}

static bool ApplySystemKeyRepeatFilterKeys() {
    int configuredStartDelay = ClampConfiguredKeyRepeatStartDelayValue(g_config.keyRepeatStartDelay, true);
    int configuredRepeatDelay = ClampConfiguredKeyRepeatDelayValue(g_config.keyRepeatDelay, true);

    if (configuredStartDelay == -1 && configuredRepeatDelay == -1) {
        RestoreKeyRepeatSettings();
        return true;
    }

    FILTERKEYS targetFilterKeys = { sizeof(FILTERKEYS) };
    targetFilterKeys.dwFlags = FKF_FILTERKEYSON;
    targetFilterKeys.iWaitMSec = 0;
    targetFilterKeys.iDelayMSec = (configuredStartDelay >= 0) ? configuredStartDelay : GetSystemDefaultKeyRepeatStartDelayMs();
    targetFilterKeys.iRepeatMSec = (configuredRepeatDelay >= 0) ? configuredRepeatDelay : GetSystemDefaultKeyRepeatDelayMs();
    targetFilterKeys.iBounceMSec = 0;

    if (SystemParametersInfo(SPI_SETFILTERKEYS, sizeof(FILTERKEYS), &targetFilterKeys, 0)) {
        g_filterKeysApplied.store(true, std::memory_order_release);
        Log("Applied FILTERKEYS settings: iDelayMSec=" + std::to_string(targetFilterKeys.iDelayMSec) +
            ", iRepeatMSec=" + std::to_string(targetFilterKeys.iRepeatMSec));
        return true;
    }

    Log("WARNING: Failed to apply FILTERKEYS settings: iDelayMSec=" + std::to_string(targetFilterKeys.iDelayMSec) +
        ", iRepeatMSec=" + std::to_string(targetFilterKeys.iRepeatMSec));
    return false;
}

void ApplyKeyRepeatSettings() {
    ResetLocalKeyRepeatState(g_subclassedHwnd.load(std::memory_order_acquire));
    if (!ShouldOwnGlobalInputState()) {
        RestoreKeyRepeatSettings();
        return;
    }

    if (!g_originalFilterKeysCaptured.load(std::memory_order_acquire)) {
        SaveOriginalKeyRepeatSettings();
    }

    (void)ApplySystemKeyRepeatFilterKeys();
}

void RestoreKeyRepeatSettings() {
    if (g_filterKeysApplied.load()) {
        if (SystemParametersInfo(SPI_SETFILTERKEYS, sizeof(FILTERKEYS), &g_originalFilterKeys, 0)) {
            Log("Restored original FILTERKEYS settings");
        } else {
            Log("WARNING: Failed to restore FILTERKEYS settings");
        }
        g_filterKeysApplied.store(false);
    }
}

typedef BOOL(WINAPI* WGLSWAPBUFFERS)(HDC);
WGLSWAPBUFFERS owglSwapBuffers = NULL;
WGLSWAPBUFFERS g_owglSwapBuffersThirdParty = NULL;
std::atomic<void*> g_wglSwapBuffersThirdPartyHookTarget{ nullptr };
typedef BOOL(WINAPI* SETCURSORPOSPROC)(int, int);
SETCURSORPOSPROC oSetCursorPos = NULL;
SETCURSORPOSPROC g_oSetCursorPosThirdParty = NULL;
std::atomic<void*> g_setCursorPosThirdPartyHookTarget{ nullptr };
typedef BOOL(WINAPI* GETCLIENTRECTPROC)(HWND, LPRECT);
GETCLIENTRECTPROC oGetClientRect = NULL;
std::atomic<HWND> g_lastLegacyLwjglResizeSyncHwnd{ NULL };
std::atomic<int> g_lastLegacyLwjglResizeSyncWidth{ 0 };
std::atomic<int> g_lastLegacyLwjglResizeSyncHeight{ 0 };
typedef BOOL(WINAPI* CLIPCURSORPROC)(const RECT*);
CLIPCURSORPROC oClipCursor = NULL;
CLIPCURSORPROC g_oClipCursorThirdParty = NULL;
std::atomic<void*> g_clipCursorThirdPartyHookTarget{ nullptr };
typedef HCURSOR(WINAPI* SETCURSORPROC)(HCURSOR);
SETCURSORPROC oSetCursor = NULL;
SETCURSORPROC g_oSetCursorThirdParty = NULL;
std::atomic<void*> g_setCursorThirdPartyHookTarget{ nullptr };
typedef void(WINAPI* GLVIEWPORTPROC)(GLint x, GLint y, GLsizei width, GLsizei height);
GLVIEWPORTPROC oglViewport = NULL;
typedef void(APIENTRY* GLBINDFRAMEBUFFERPROC)(GLenum target, GLuint framebuffer);
GLBINDFRAMEBUFFERPROC oglBindFramebuffer = NULL;
GLBINDFRAMEBUFFERPROC g_oglBindFramebufferDriver = NULL;

// Additional glViewport hook chains for compatibility with driver-level entrypoints and
GLVIEWPORTPROC g_oglViewportDriver = NULL;
GLVIEWPORTPROC g_oglViewportThirdParty = NULL;
std::atomic<void*> g_glViewportDriverHookTarget{ nullptr };
std::atomic<void*> g_glViewportThirdPartyHookTarget{ nullptr };

// Thread-local flag to track if glViewport is being called from our own code
thread_local bool g_internalViewportCall = false;

struct PendingViewportRestoreEntry {
    bool active = false;
    uint64_t sequence = 0;
    GLint x = 0;
    GLint y = 0;
    GLsizei width = 0;
    GLsizei height = 0;
};

thread_local PendingViewportRestoreEntry g_pendingViewportRestore;
thread_local uint64_t g_glStateHookSequence = 0;

struct TextureBindingCacheEntry {
    HGLRC context = nullptr;
    GLuint texture2D = 0;
    bool valid = false;
};


std::atomic<int> g_glViewportHookCount{ 0 };
std::atomic<bool> g_glViewportHookedViaGLEW{ false };
std::atomic<bool> g_glViewportHookedViaWGL{ false };

typedef void(WINAPI* GLBLITNAMEDFRAMEBUFFERPROC)(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1,
                                                 GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask,
                                                 GLenum filter);
GLBLITNAMEDFRAMEBUFFERPROC oglBlitNamedFramebuffer = NULL;

typedef void(APIENTRY* GLNAMEDFRAMEBUFFERTEXTUREPROC)(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level);
GLNAMEDFRAMEBUFFERTEXTUREPROC oglNamedFramebufferTexture = NULL;

typedef void(APIENTRY* PFNGLBLITFRAMEBUFFERPROC_HOOK)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                                      GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
PFNGLBLITFRAMEBUFFERPROC_HOOK oglBlitFramebuffer = NULL;
PFNGLBLITFRAMEBUFFERPROC_HOOK g_oglBlitFramebufferDriver = NULL;
PFNGLBLITFRAMEBUFFERPROC_HOOK g_oglBlitFramebufferThirdParty = NULL;
std::atomic<void*> g_glBlitFramebufferThirdPartyHookTarget{ nullptr };
std::atomic<bool> g_glBlitFramebufferHooked{ false };

typedef void (APIENTRY* GLBINDTEXTUREPROC)(GLenum target, GLuint texture);
GLBINDTEXTUREPROC oglBindTexture = NULL;
GLBINDTEXTUREPROC g_oglBindTextureDriver = NULL;

struct InternalViewportCallScope {
    bool previous = false;

    InternalViewportCallScope() : previous(g_internalViewportCall) {
        g_internalViewportCall = true;
    }

    ~InternalViewportCallScope() {
        g_internalViewportCall = previous;
    }
};

static inline uint64_t NextGlStateHookSequence() {
    return ++g_glStateHookSequence;
}

static inline void ViewportDirect(GLint x, GLint y, GLsizei width, GLsizei height) {
    GLVIEWPORTPROC next = g_oglViewportDriver ? g_oglViewportDriver : oglViewport;
    InternalViewportCallScope scope;
    if (next) {
        next(x, y, width, height);
        return;
    }

    glViewport(x, y, width, height);
}

static __forceinline bool IsDynamicMemoryCaller(void* caller_address) {
    if (!caller_address) {
        return false;
    }

    struct CallerCacheEntry {
        void* caller = nullptr;
        bool isDynamic = false;
    };

    constexpr size_t kCallerCacheSize = 32;
    constexpr size_t kCallerCacheMask = kCallerCacheSize - 1;
    thread_local std::array<CallerCacheEntry, kCallerCacheSize> callerCache{};

    const size_t cacheIndex = (reinterpret_cast<uintptr_t>(caller_address) >> 4) & kCallerCacheMask;
    CallerCacheEntry& entry = callerCache[cacheIndex];
    if (entry.caller == caller_address) {
        return entry.isDynamic;
    }

    PVOID baseOfImage = nullptr;
    const bool isDynamic = (RtlPcToFileHeader(caller_address, &baseOfImage) == NULL);

    entry.caller = caller_address;
    entry.isDynamic = isDynamic;
    return isDynamic;
}

static __forceinline bool IsLegacyLwjglCaller(void* caller_address) {
    if (!caller_address) {
        return false;
    }

    struct CallerCacheEntry {
        void* caller = nullptr;
        bool isLegacyLwjgl = false;
    };

    constexpr size_t kCallerCacheSize = 32;
    constexpr size_t kCallerCacheMask = kCallerCacheSize - 1;
    thread_local std::array<CallerCacheEntry, kCallerCacheSize> callerCache{};

    const size_t cacheIndex = (reinterpret_cast<uintptr_t>(caller_address) >> 4) & kCallerCacheMask;
    CallerCacheEntry& entry = callerCache[cacheIndex];
    if (entry.caller == caller_address) {
        return entry.isLegacyLwjgl;
    }

    PVOID baseOfImage = nullptr;
    bool isLegacyLwjgl = false;
    if (RtlPcToFileHeader(caller_address, &baseOfImage) != NULL && baseOfImage != nullptr) {
        WCHAR modulePath[MAX_PATH] = {};
        DWORD len = GetModuleFileNameW(static_cast<HMODULE>(baseOfImage), modulePath, static_cast<DWORD>(std::size(modulePath)));
        if (len > 0) {
            std::wstring modulePathLower(modulePath, len);
            for (wchar_t& ch : modulePathLower) {
                ch = static_cast<wchar_t>(towlower(ch));
            }
            isLegacyLwjgl = modulePathLower.find(L"lwjgl") != std::wstring::npos;
        }
    }

    entry.caller = caller_address;
    entry.isLegacyLwjgl = isLegacyLwjgl;
    return isLegacyLwjgl;
}

static bool ShouldUseLegacyRequestedClientRect(HWND hwnd, int actualWidth, int actualHeight, int& outWidth, int& outHeight) {
    outWidth = 0;
    outHeight = 0;

    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    const HWND trackedHwnd = g_minecraftHwnd.load(std::memory_order_acquire);
    const HWND subclassedHwnd = g_subclassedHwnd.load(std::memory_order_acquire);
    if (hwnd != trackedHwnd && hwnd != subclassedHwnd) {
        return false;
    }

    int requestedWidth = 0;
    int requestedHeight = 0;
    int previousRequestedWidth = 0;
    int previousRequestedHeight = 0;
    if (!GetRecentRequestedWindowClientResizes(requestedWidth, requestedHeight, previousRequestedWidth, previousRequestedHeight)) {
        return false;
    }

    if (requestedWidth <= 0 || requestedHeight <= 0) {
        return false;
    }

    if (actualWidth <= 0 || actualHeight <= 0) {
        return false;
    }

    if (requestedWidth == actualWidth && requestedHeight == actualHeight) {
        return false;
    }

    outWidth = requestedWidth;
    outHeight = requestedHeight;
    return true;
}

static __forceinline bool IsDisallowedThirdPartySwapCaller(void* caller_address) {
    void* installedHookTarget = g_wglSwapBuffersThirdPartyHookTarget.load(std::memory_order_acquire);
    if (installedHookTarget) {
        return false;
    }

    if (!caller_address) {
        return true;
    }

    PVOID baseOfImage = nullptr;
    if (RtlPcToFileHeader(caller_address, &baseOfImage) == NULL || !baseOfImage) {
        return true;
    }

    return !HookChain::IsAllowedSwapBuffersThirdPartyHookAddress(baseOfImage) &&
           !HookChain::IsAllowedSwapBuffersThirdPartyHookAddress(caller_address);
}

    void APIENTRY hkglBlitFramebuffer(GLint srcX0,
                          GLint srcY0,
                          GLint srcX1,
                          GLint srcY1,
                          GLint dstX0,
                          GLint dstY0,
                          GLint dstX1,
                          GLint dstY1,
                          GLbitfield mask,
                          GLenum filter);
    void APIENTRY hkglBlitFramebuffer_Driver(GLint srcX0,
                              GLint srcY0,
                              GLint srcX1,
                              GLint srcY1,
                              GLint dstX0,
                              GLint dstY0,
                              GLint dstX1,
                              GLint dstY1,
                              GLbitfield mask,
                              GLenum filter);
    void APIENTRY hkglBlitFramebuffer_ThirdParty(GLint srcX0,
                               GLint srcY0,
                               GLint srcX1,
                               GLint srcY1,
                               GLint dstX0,
                               GLint dstY0,
                               GLint dstX1,
                               GLint dstY1,
                               GLbitfield mask,
                               GLenum filter);

static bool IsReadableExecutablePointer(const void* address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }

    if (mbi.State != MEM_COMMIT) {
        return false;
    }

    const DWORD protect = mbi.Protect & 0xFF;
    return protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE ||
           protect == PAGE_EXECUTE_WRITECOPY || protect == PAGE_READONLY || protect == PAGE_READWRITE ||
           protect == PAGE_WRITECOPY;
}

static bool IsAbsoluteJumpStub(const uint8_t* bytes) {
    if (!bytes) {
        return false;
    }

    return (bytes[0] == 0xEB) || (bytes[0] == 0xE9) || (bytes[0] == 0xFF && bytes[1] == 0x25) ||
           (bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0) ||
           (bytes[0] == 0x49 && bytes[1] == 0xBB && bytes[10] == 0x41 && bytes[11] == 0xFF && bytes[12] == 0xE3);
}

static bool TryResolveJumpTarget(void* current, void*& next) {
    next = nullptr;
    if (!current || !IsReadableExecutablePointer(current)) {
        return false;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(current);
    if (bytes[0] == 0xEB) {
        const int8_t rel = *reinterpret_cast<const int8_t*>(bytes + 1);
        next = const_cast<uint8_t*>(bytes + 2 + rel);
    } else if (bytes[0] == 0xE9) {
        const int32_t rel = *reinterpret_cast<const int32_t*>(bytes + 1);
        next = const_cast<uint8_t*>(bytes + 5 + rel);
    } else if (bytes[0] == 0xFF && bytes[1] == 0x25) {
        const int32_t disp = *reinterpret_cast<const int32_t*>(bytes + 2);
        const uint8_t* ripNext = bytes + 6;
        const uint8_t* slot = ripNext + disp;
        if (!IsReadableExecutablePointer(slot)) {
            return false;
        }
        next = *reinterpret_cast<void* const*>(slot);
    } else if (bytes[0] == 0x48 && bytes[1] == 0xB8 && bytes[10] == 0xFF && bytes[11] == 0xE0) {
        next = *reinterpret_cast<void* const*>(bytes + 2);
    } else if (bytes[0] == 0x49 && bytes[1] == 0xBB && bytes[10] == 0x41 && bytes[11] == 0xFF && bytes[12] == 0xE3) {
        next = *reinterpret_cast<void* const*>(bytes + 2);
    }

    return next != nullptr;
}

static void* ResolveBlitFramebufferThirdPartyTarget(void* startAddress) {
    void* current = startAddress;
    for (int depth = 0; depth < 8; ++depth) {
        if (!current || !IsReadableExecutablePointer(current) || !IsAbsoluteJumpStub(reinterpret_cast<const uint8_t*>(current))) {
            return nullptr;
        }

        void* next = nullptr;
        if (!TryResolveJumpTarget(current, next) || !next) {
            return nullptr;
        }

        if (next != reinterpret_cast<void*>(&hkglBlitFramebuffer) &&
            next != reinterpret_cast<void*>(&hkglBlitFramebuffer_Driver) &&
            next != reinterpret_cast<void*>(&hkglBlitFramebuffer_ThirdParty) &&
            next != reinterpret_cast<void*>(oglBlitFramebuffer) &&
            next != reinterpret_cast<void*>(g_oglBlitFramebufferDriver)) {
            return next;
        }

        current = next;
    }

    return nullptr;
}

void APIENTRY BindTextureDirect(GLenum target, GLuint texture) {
    GLBINDTEXTUREPROC next = oglBindTexture ? oglBindTexture : g_oglBindTextureDriver;
    if (next) {
        next(target, texture);
        return;
    }
    glBindTexture(target, texture);
}

static bool GetTexture2DLevel0Size(GLuint texture, int& outWidth, int& outHeight) {
    outWidth = 0;
    outHeight = 0;
    if (texture == 0 || texture == UINT_MAX || glIsTexture(texture) != GL_TRUE) {
        return false;
    }

    GLint previousActiveTexture = 0;
    GLint previousTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);

    BindTextureDirect(GL_TEXTURE_2D, texture);
    GLint textureWidth = 0;
    GLint textureHeight = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &textureWidth);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &textureHeight);

    BindTextureDirect(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture));
    glActiveTexture(previousActiveTexture);

    if (textureWidth <= 0 || textureHeight <= 0) {
        return false;
    }

    outWidth = textureWidth;
    outHeight = textureHeight;
    return true;
}

void BlitFramebufferDirect(GLint srcX0,
                           GLint srcY0,
                           GLint srcX1,
                           GLint srcY1,
                           GLint dstX0,
                           GLint dstY0,
                           GLint dstX1,
                           GLint dstY1,
                           GLbitfield mask,
                           GLenum filter) {
    PFNGLBLITFRAMEBUFFERPROC_HOOK next = g_oglBlitFramebufferDriver ? g_oglBlitFramebufferDriver : oglBlitFramebuffer;
    if (next) {
        next(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
        return;
    }

    glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

static bool GetLatestViewportForHook(int& outModeW, int& outModeH, bool& outStretchEnabled, int& outStretchX, int& outStretchY,
                                     int& outStretchW, int& outStretchH);

static bool BoundTextureMatchesPreviousFrameModeSize() {
    int modeW = 0;
    int modeH = 0;
    bool stretchEnabled = false;
    int stretchX = 0;
    int stretchY = 0;
    int stretchW = 0;
    int stretchH = 0;
    if (!GetLatestViewportForHook(modeW, modeH, stretchEnabled, stretchX, stretchY, stretchW, stretchH)) {
        return false;
    }

    if (stretchW <= 0 || stretchH <= 0) {
        return false;
    }

    GLint textureWidth = 0;
    GLint textureHeight = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &textureWidth);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &textureHeight);
    return textureWidth == stretchW && textureHeight == stretchH;
}


static inline void BindTextureHook_Impl(GLBINDTEXTUREPROC next, GLenum target, GLuint texture) {
    if (next) next(target, texture);
    if (target == GL_TEXTURE_2D && texture != 0) {
        static thread_local const DWORD s_currentThreadId = GetCurrentThreadId();
        const DWORD lastSwapThreadId = g_lastSwapBuffersThreadId.load(std::memory_order_acquire);
        if (lastSwapThreadId != 0 && s_currentThreadId == lastSwapThreadId && BoundTextureMatchesPreviousFrameModeSize()) {
            g_lastTrackedGameTextureBindId.store(texture, std::memory_order_release);
        }
    }

}

void APIENTRY hkglBindTexture(GLenum target, GLuint texture) {
    void* caller_address = _ReturnAddress();
    if (!IsDynamicMemoryCaller(caller_address)) {
        if (oglBindTexture) {
            oglBindTexture(target, texture);
            return;
        }
        glBindTexture(target, texture);
        return;
    }

    BindTextureHook_Impl(oglBindTexture, target, texture);
}

void APIENTRY hkglBindTexture_Driver(GLenum target, GLuint texture) {
    void* caller_address = _ReturnAddress();
    if (!IsDynamicMemoryCaller(caller_address)) {
        if (g_oglBindTextureDriver) {
            g_oglBindTextureDriver(target, texture);
            return;
        }
        if (oglBindTexture) {
            oglBindTexture(target, texture);
            return;
        }
        glBindTexture(target, texture);
        return;
    }

    BindTextureHook_Impl(g_oglBindTextureDriver, target, texture);
}

static inline bool BindFramebufferTargetAffectsDraw(GLenum target) {
    return target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER;
}

static inline void BindFramebufferHook_Impl(GLBINDFRAMEBUFFERPROC next, GLenum target, GLuint framebuffer) {
    if (!next) {
        return;
    }

    const uint64_t hookSequence = NextGlStateHookSequence();
    const PendingViewportRestoreEntry pendingRestore = g_pendingViewportRestore;
    const bool targetAffectsDraw = BindFramebufferTargetAffectsDraw(target);

    const bool shouldRestoreViewport = pendingRestore.active && targetAffectsDraw && framebuffer != 0 &&
                                       hookSequence <= (pendingRestore.sequence + 2);

    next(target, framebuffer);

    if (pendingRestore.active && targetAffectsDraw) {
        g_pendingViewportRestore.active = false;
    }

    if (shouldRestoreViewport) {
        ViewportDirect(pendingRestore.x, pendingRestore.y, pendingRestore.width, pendingRestore.height);
    }
}

void APIENTRY hkglBindFramebuffer(GLenum target, GLuint framebuffer) {
    if (!oglBindFramebuffer) {
        return;
    }

    void* caller_address = _ReturnAddress();
    if (!IsDynamicMemoryCaller(caller_address)) {
        oglBindFramebuffer(target, framebuffer);
        return;
    }

    BindFramebufferHook_Impl(oglBindFramebuffer, target, framebuffer);
}

void APIENTRY hkglBindFramebuffer_Driver(GLenum target, GLuint framebuffer) {
    if (!g_oglBindFramebufferDriver) {
        return;
    }

    void* caller_address = _ReturnAddress();
    if (!IsDynamicMemoryCaller(caller_address)) {
        g_oglBindFramebufferDriver(target, framebuffer);
        return;
    }

    BindFramebufferHook_Impl(g_oglBindFramebufferDriver, target, framebuffer);
}

typedef void (*GLFWSETINPUTMODE)(void* window, int mode, int value);
GLFWSETINPUTMODE oglfwSetInputMode = NULL;
GLFWSETINPUTMODE g_oglfwSetInputModeThirdParty = NULL;
std::atomic<void*> g_glfwSetInputModeThirdPartyHookTarget{ nullptr };

typedef UINT(WINAPI* GETRAWINPUTDATAPROC)(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
GETRAWINPUTDATAPROC oGetRawInputData = NULL;
GETRAWINPUTDATAPROC g_oGetRawInputDataThirdParty = NULL;
std::atomic<void*> g_getRawInputDataThirdPartyHookTarget{ nullptr };

static const RECT* ResolveClipCursorRect(const RECT* lpRect, RECT& resolvedRect) {
    if (g_config.confineCursor) {
        HWND hwnd = g_minecraftHwnd.load();
        if (IsWindowInForegroundTree(hwnd) && GetWindowClientRectInScreen(hwnd, resolvedRect)) { return &resolvedRect; }
    }

    if (g_showGui.load()) { return NULL; }

    if (g_gameVersion < GameVersion(1, 13, 0) && g_config.allowCursorEscape) { return NULL; }

    return lpRect;
}

BOOL ClipCursorDirect(const RECT* lpRect) {
    CLIPCURSORPROC directProc = oClipCursor ? oClipCursor : ::ClipCursor;
    if (!directProc) return FALSE;
    return directProc(lpRect);
}

bool ApplyConfineCursorToGameWindow() {
    if (!g_config.confineCursor) { return false; }

    RECT clipRect{};
    HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
    if (!IsWindowInForegroundTree(hwnd)) { return false; }
    if (!GetWindowClientRectInScreen(hwnd, clipRect)) { return false; }

    return ClipCursorDirect(&clipRect) != FALSE;
}

static BOOL ClipCursorHook_Impl(CLIPCURSORPROC next, const RECT* lpRect) {
    if (!next) return FALSE;

    RECT resolvedRect{};
    return next(ResolveClipCursorRect(lpRect, resolvedRect));
}

BOOL WINAPI hkClipCursor(const RECT* lpRect) { return ClipCursorHook_Impl(oClipCursor, lpRect); }

BOOL WINAPI hkClipCursor_ThirdParty(const RECT* lpRect) {
    CLIPCURSORPROC next = g_oClipCursorThirdParty ? g_oClipCursorThirdParty : oClipCursor;
    return ClipCursorHook_Impl(next, lpRect);
}

static HCURSOR SetCursorHook_Impl(SETCURSORPROC next, HCURSOR hCursor) {
    if (!next) return NULL;

    if (g_gameVersion >= GameVersion(1, 13, 0)) { return next(hCursor); }

    if (g_showGui.load()) {
        const std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
        const CursorTextures::CursorData* cursorData = CursorTextures::GetSelectedCursor(localGameState, 64);
        if (cursorData && cursorData->hCursor) { return next(cursorData->hCursor); }
    }

    if (hCursor == NULL || g_specialCursorHandle.load() != NULL) { return next(hCursor); }

    static std::mutex s_scannedCursorHandlesMutex;
    static std::unordered_set<HCURSOR> s_scannedCursorHandles;
    {
        std::lock_guard<std::mutex> lock(s_scannedCursorHandlesMutex);
        if (!s_scannedCursorHandles.insert(hCursor).second) {
            return next(hCursor);
        }
    }

    ICONINFO ii = { sizeof(ICONINFO) };
    if (GetIconInfo(hCursor, &ii)) {
        BITMAP bitmask = {};
        GetObject(ii.hbmMask, sizeof(BITMAP), &bitmask);

        std::string maskHash = "N/A";
        if (bitmask.bmWidth > 0 && bitmask.bmHeight > 0) {
            size_t bufferSize = bitmask.bmWidth * bitmask.bmHeight;
            std::vector<BYTE> maskPixels(bufferSize, 0);
            if (GetBitmapBits(ii.hbmMask, static_cast<LONG>(bufferSize), maskPixels.data()) > 0) {
                uint32_t hash = 0;
                for (BYTE pixel : maskPixels) { hash = ((hash << 5) + hash) ^ pixel; }
                std::ostringstream oss;
                oss << std::hex << hash;
                maskHash = oss.str();
            }
        }

        Log("hkSetCursor: maskHash = " + maskHash);

        if (maskHash == "773ff800") {
            Log("hkSetCursor: Detected special cursor (maskHash=773ff800), caching for later use");
            g_specialCursorHandle.store(hCursor);
        }

        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
    }

    return next(hCursor);
}

HCURSOR WINAPI hkSetCursor(HCURSOR hCursor) { return SetCursorHook_Impl(oSetCursor, hCursor); }

HCURSOR WINAPI hkSetCursor_ThirdParty(HCURSOR hCursor) {
    SETCURSORPROC next = g_oSetCursorThirdParty ? g_oSetCursorThirdParty : oSetCursor;
    return SetCursorHook_Impl(next, hCursor);
}
// OBS redirect now plugs into the main glBlitFramebuffer hook below.

static std::atomic<int> lastViewportW{ 0 };
static std::atomic<int> lastViewportH{ 0 };

static constexpr int kViewportHookRecentModeHistory = 6;

struct ViewportHookCache {
    uint64_t configVersion = UINT64_MAX;
    std::string modeId;
    int screenW = 0;
    int screenH = 0;
    int modeW = 0;
    int modeH = 0;
    bool stretchEnabled = false;
    int stretchX = 0;
    int stretchY = 0;
    int stretchW = 0;
    int stretchH = 0;
    int recentModeW[kViewportHookRecentModeHistory] = {};
    int recentModeH[kViewportHookRecentModeHistory] = {};
    int recentModeCount = 0;
    bool valid = false;
};

static ViewportHookCache& GetViewportHookCache() {
    thread_local ViewportHookCache s_cache;
    return s_cache;
}

static void RememberViewportHookModeSize(ViewportHookCache& cache, int modeW, int modeH) {
    if (modeW < 1 || modeH < 1) { return; }

    int existingIndex = -1;
    for (int i = 0; i < cache.recentModeCount; ++i) {
        if (cache.recentModeW[i] == modeW && cache.recentModeH[i] == modeH) {
            existingIndex = i;
            break;
        }
    }

    if (existingIndex >= 0) {
        for (int i = existingIndex; i + 1 < cache.recentModeCount; ++i) {
            cache.recentModeW[i] = cache.recentModeW[i + 1];
            cache.recentModeH[i] = cache.recentModeH[i + 1];
        }
        cache.recentModeW[cache.recentModeCount - 1] = modeW;
        cache.recentModeH[cache.recentModeCount - 1] = modeH;
        return;
    }

    if (cache.recentModeCount < kViewportHookRecentModeHistory) {
        cache.recentModeW[cache.recentModeCount] = modeW;
        cache.recentModeH[cache.recentModeCount] = modeH;
        ++cache.recentModeCount;
        return;
    }

    for (int i = 1; i < kViewportHookRecentModeHistory; ++i) {
        cache.recentModeW[i - 1] = cache.recentModeW[i];
        cache.recentModeH[i - 1] = cache.recentModeH[i];
    }
    cache.recentModeW[kViewportHookRecentModeHistory - 1] = modeW;
    cache.recentModeH[kViewportHookRecentModeHistory - 1] = modeH;
}

static bool MatchesRecentViewportHookModeSize(const ViewportHookCache& cache, int modeW, int modeH) {
    for (int i = 0; i < cache.recentModeCount; ++i) {
        if (cache.recentModeW[i] == modeW && cache.recentModeH[i] == modeH) { return true; }
    }
    return false;
}

static bool GetLatestViewportForHook(int& outModeW, int& outModeH, bool& outStretchEnabled, int& outStretchX, int& outStretchY,
                                     int& outStretchW, int& outStretchH) {
    ViewportHookCache& s_cache = GetViewportHookCache();

    const uint64_t configVersion = g_configSnapshotVersion.load(std::memory_order_acquire);
    const int modeIdx = g_currentModeIdIndex.load(std::memory_order_acquire);
    const std::string& currentModeId = g_modeIdBuffers[modeIdx];

    const int screenW = (std::max)(1, GetCachedWindowWidth());
    const int screenH = (std::max)(1, GetCachedWindowHeight());

    if (s_cache.valid && s_cache.configVersion == configVersion && s_cache.screenW == screenW && s_cache.screenH == screenH &&
        s_cache.modeId == currentModeId) {
        outModeW = s_cache.modeW;
        outModeH = s_cache.modeH;
        outStretchEnabled = s_cache.stretchEnabled;
        outStretchX = s_cache.stretchX;
        outStretchY = s_cache.stretchY;
        outStretchW = s_cache.stretchW;
        outStretchH = s_cache.stretchH;
        return true;
    }

    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) { return false; }

    const ModeConfig* mode = GetModeFromSnapshotOrFallback(*cfgSnap, currentModeId);
    if (!mode) { return false; }

    if (s_cache.valid && s_cache.modeId != currentModeId) {
        s_cache.recentModeCount = 0;
    } else if (s_cache.valid) {
        RememberViewportHookModeSize(s_cache, s_cache.modeW, s_cache.modeH);
    }

    // Single source of truth: logic-thread-recalculated mode dimensions.
    // Do not re-run relative/expression math in the hook; that can introduce
    // independent rounding/timing drift versus WM_SIZE enforcement.
    int modeW = mode->width;
    int modeH = mode->height;

    if (modeW < 1 || modeH < 1) { return false; }

    s_cache.configVersion = configVersion;
    s_cache.modeId = currentModeId;
    s_cache.screenW = screenW;
    s_cache.screenH = screenH;
    s_cache.modeW = modeW;
    s_cache.modeH = modeH;
    s_cache.stretchEnabled = mode->stretch.enabled;
    if (mode->stretch.enabled) {
        if (EqualsIgnoreCase(mode->id, "Fullscreen")) {
            s_cache.stretchX = 0;
            s_cache.stretchY = 0;
            s_cache.stretchW = screenW;
            s_cache.stretchH = screenH;
        } else {
            s_cache.stretchX = mode->stretch.x;
            s_cache.stretchY = mode->stretch.y;
            s_cache.stretchW = mode->stretch.width;
            s_cache.stretchH = mode->stretch.height;
        }
    } else {
        s_cache.stretchX = GetCenteredAxisOffset(screenW, modeW);
        s_cache.stretchY = GetCenteredAxisOffset(screenH, modeH);
        s_cache.stretchW = modeW;
        s_cache.stretchH = modeH;
    }

    s_cache.valid = true;

    outModeW = s_cache.modeW;
    outModeH = s_cache.modeH;
    outStretchEnabled = s_cache.stretchEnabled;
    outStretchX = s_cache.stretchX;
    outStretchY = s_cache.stretchY;
    outStretchW = s_cache.stretchW;
    outStretchH = s_cache.stretchH;

    return true;
}

static inline void ViewportHook_Impl(GLVIEWPORTPROC next, GLint x, GLint y, GLsizei width, GLsizei height) {
    if (!next) return;

    if (g_internalViewportCall) {
        return next(x, y, width, height);
    }

    const uint64_t hookSequence = NextGlStateHookSequence();
    g_pendingViewportRestore.active = false;

    // Lock-free read of transition snapshot
    const ViewportTransitionSnapshot& transitionSnap =
        g_viewportTransitionSnapshots[g_viewportTransitionSnapshotIndex.load(std::memory_order_acquire)];
    bool isTransitionActive = transitionSnap.active;
    auto hookConfigSnap = GetConfigSnapshot();
    const bool hideAnimationsInGame = hookConfigSnap ? hookConfigSnap->hideAnimationsInGame : g_config.hideAnimationsInGame;
    const bool cutGameViewportTransition = isTransitionActive && hideAnimationsInGame;

    // Lock-free read of cached mode viewport data (updated by logic_thread)
    const CachedModeViewport& cachedMode = g_viewportModeCache[g_viewportModeCacheIndex.load(std::memory_order_acquire)];

    // The snapshot is updated synchronously on mode switch, while cache has ~16ms lag.
    int modeWidth, modeHeight;
    bool stretchEnabled;
    int stretchX, stretchY, stretchWidth, stretchHeight;

    if (isTransitionActive && !cutGameViewportTransition) {
        modeWidth = transitionSnap.toNativeWidth;
        modeHeight = transitionSnap.toNativeHeight;
        stretchEnabled = true;
        stretchX = transitionSnap.toX;
        stretchY = transitionSnap.toY;
        stretchWidth = transitionSnap.toWidth;
        stretchHeight = transitionSnap.toHeight;
    } else if (GetLatestViewportForHook(modeWidth, modeHeight, stretchEnabled, stretchX, stretchY, stretchWidth, stretchHeight)) {
        // Use live, recalculated dimensions so WM_SIZE-driven relative/expression updates
        // are reflected immediately even before the periodic viewport cache refresh.
    } else if (cachedMode.valid) {
        modeWidth = cachedMode.width;
        modeHeight = cachedMode.height;
        stretchEnabled = cachedMode.stretchEnabled;
        stretchX = cachedMode.stretchX;
        stretchY = cachedMode.stretchY;
        stretchWidth = cachedMode.stretchWidth;
        stretchHeight = cachedMode.stretchHeight;
    } else {
        return next(x, y, width, height);
    }

    const ViewportHookCache& hookCache = GetViewportHookCache();
    const int lastViewportWValue = lastViewportW.load(std::memory_order_relaxed);
    const int lastViewportHValue = lastViewportH.load(std::memory_order_relaxed);
    int requestedViewportW = 0;
    int requestedViewportH = 0;
    int previousRequestedViewportW = 0;
    int previousRequestedViewportH = 0;
    GetRecentRequestedWindowClientResizes(requestedViewportW, requestedViewportH, previousRequestedViewportW, previousRequestedViewportH);

    bool posValid = x == 0 && y == 0;
    bool dimsMatch = false;
    if (isTransitionActive && !cutGameViewportTransition) {
        dimsMatch = (width == transitionSnap.fromNativeWidth && height == transitionSnap.fromNativeHeight) ||
                    (width == transitionSnap.toNativeWidth && height == transitionSnap.toNativeHeight);

        if (!dimsMatch) {
            dimsMatch = (width == modeWidth && height == modeHeight) ||
                        (cachedMode.valid && width == cachedMode.width && height == cachedMode.height);
        }
    } else {
        dimsMatch = (width == modeWidth && height == modeHeight) ||
                    (width == lastViewportWValue && height == lastViewportHValue) ||
                    (cachedMode.valid && width == cachedMode.width && height == cachedMode.height) ||
                    (width == requestedViewportW && height == requestedViewportH) ||
                    (width == previousRequestedViewportW && height == previousRequestedViewportH) ||
                    MatchesRecentViewportHookModeSize(hookCache, static_cast<int>(width), static_cast<int>(height));

        if (isTransitionActive && !dimsMatch) {
            dimsMatch = (width == transitionSnap.toNativeWidth && height == transitionSnap.toNativeHeight);
        }
    }

    if (!posValid || !dimsMatch) {
        /*Log("Returning because viewport parameters don't match mode (x=" + std::to_string(x) + ", y=" + std::to_string(y) +
            ", width=" + std::to_string(width) + ", height=" + std::to_string(height) +
            "), lastViewportW=" + std::to_string(lastViewportWValue) + ", lastViewportH=" + std::to_string(lastViewportHValue) +
            ", requestedViewportW=" + std::to_string(requestedViewportW) + ", requestedViewportH=" + std::to_string(requestedViewportH) +
            ", previousRequestedViewportW=" + std::to_string(previousRequestedViewportW) +
            ", previousRequestedViewportH=" + std::to_string(previousRequestedViewportH) +
            ", cachedModeWidth=" + std::to_string(cachedMode.width) + ", cachedModeHeight=" + std::to_string(cachedMode.height) +
            ", modeWidth=" + std::to_string(modeWidth) + ", modeHeight=" + std::to_string(modeHeight) +
            ", recentModeCount=" + std::to_string(hookCache.recentModeCount) + ")");*/
        return next(x, y, width, height);
    }

    GLint drawFBO = 0;
    GLint currentTextureBinding = 0;

    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFBO);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &currentTextureBinding);
    const GLuint currentTexture = static_cast<GLuint>(currentTextureBinding);

    const bool isLegacyVersion = g_gameVersion < GameVersion(1, 17, 0);
    const bool shouldBypassViewportHook = isLegacyVersion ?
        (drawFBO != 0) :
        (currentTexture != 0 || drawFBO != 0);
    if (shouldBypassViewportHook) {
        return next(x, y, width, height);
    }

    if (!g_showGui.load(std::memory_order_acquire)) {
        // Track the latest verified in-game viewport only while the settings GUI is closed.
        // GUI rendering can issue its own viewport calls, and carrying those sizes into
        // post-close mouse translation can desync cursor mapping from the game surface.
        lastViewportW.store(static_cast<int>(width), std::memory_order_relaxed);
        lastViewportH.store(static_cast<int>(height), std::memory_order_relaxed);
    }

    const int screenW = GetCachedWindowWidth();
    const int screenH = GetCachedWindowHeight();
    if (screenW <= 0 || screenH <= 0) {
        return next(x, y, width, height);
    }

    bool useAnimatedDimensions = transitionSnap.active && !cutGameViewportTransition;
    int animatedX = transitionSnap.currentX;
    int animatedY = transitionSnap.currentY;
    int animatedWidth = transitionSnap.currentWidth;
    int animatedHeight = transitionSnap.currentHeight;
    int targetX = transitionSnap.toX;
    int targetY = transitionSnap.toY;
    int targetWidth = transitionSnap.toWidth;
    int targetHeight = transitionSnap.toHeight;

    if (useAnimatedDimensions) {
        bool shouldSkipAnimation = hideAnimationsInGame;

        if (shouldSkipAnimation) {
            stretchX = targetX;
            stretchY = targetY;
            stretchWidth = targetWidth;
            stretchHeight = targetHeight;
        } else {
            stretchX = animatedX;
            stretchY = animatedY;
            stretchWidth = animatedWidth;
            stretchHeight = animatedHeight;
        }
    } else {
        if (!stretchEnabled) {
            stretchX = GetCenteredAxisOffset(screenW, modeWidth);
            stretchY = GetCenteredAxisOffset(screenH, modeHeight);
            stretchWidth = modeWidth;
            stretchHeight = modeHeight;
        }
    }

    // Convert Y coordinate from Windows screen space (top-left origin) to OpenGL viewport space (bottom-left origin)
    int stretchY_gl = screenH - stretchY - stretchHeight;
    /*Log("Applying viewport hook with parameters: x=" + std::to_string(stretchX) + ", y=" + std::to_string(stretchY_gl) +
        ", width=" + std::to_string(stretchWidth) + ", height=" + std::to_string(stretchHeight) +
        ", modeWidth=" + std::to_string(modeWidth) + ", modeHeight=" + std::to_string(modeHeight) +
        ", screenW=" + std::to_string(screenW) + ", screenH=" + std::to_string(screenH) +
        (useAnimatedDimensions ? ", animated" : ""));*/

    g_pendingViewportRestore.active = true;
    g_pendingViewportRestore.sequence = hookSequence;
    g_pendingViewportRestore.x = x;
    g_pendingViewportRestore.y = y;
    g_pendingViewportRestore.width = width;
    g_pendingViewportRestore.height = height;

    return next(stretchX, stretchY_gl, stretchWidth, stretchHeight);
}

void WINAPI hkglViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (!oglViewport) {
        return;
    }

    if (g_internalViewportCall) {
        oglViewport(x, y, width, height);
        return;
    }

    void* caller_address = _ReturnAddress();
    if (!IsDynamicMemoryCaller(caller_address)) {
        oglViewport(x, y, width, height);
        return;
    }

    ViewportHook_Impl(oglViewport, x, y, width, height);
}

// Driver-level glViewport hook (wglGetProcAddress / GLEW-resolved function pointer).
void WINAPI hkglViewport_Driver(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (!g_oglViewportDriver) {
        return;
    }

    if (g_internalViewportCall) {
        g_oglViewportDriver(x, y, width, height);
        return;
    }

    void* caller_address = _ReturnAddress();
    if (!IsDynamicMemoryCaller(caller_address)) {
        g_oglViewportDriver(x, y, width, height);
        return;
    }

    ViewportHook_Impl(g_oglViewportDriver, x, y, width, height);
}

void WINAPI hkglViewport_ThirdParty(GLint x, GLint y, GLsizei width, GLsizei height) {
    GLVIEWPORTPROC next = g_oglViewportThirdParty ? g_oglViewportThirdParty : (oglViewport ? oglViewport : g_oglViewportDriver);

    if (!next) {
        return;
    }

    if (g_internalViewportCall) {
        next(x, y, width, height);
        return;
    }

    void* caller_address = _ReturnAddress();

    if (!IsDynamicMemoryCaller(caller_address)) {
        next(x, y, width, height);
        return;
    }

    ViewportHook_Impl(next, x, y, width, height);
}

void WINAPI hkglBlitNamedFramebuffer(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                     GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
void APIENTRY hkglBlitFramebuffer(GLint srcX0,
                                  GLint srcY0,
                                  GLint srcX1,
                                  GLint srcY1,
                                  GLint dstX0,
                                  GLint dstY0,
                                  GLint dstX1,
                                  GLint dstY1,
                                  GLbitfield mask,
                                  GLenum filter);
void APIENTRY hkglBlitFramebuffer_Driver(GLint srcX0,
                                         GLint srcY0,
                                         GLint srcX1,
                                         GLint srcY1,
                                         GLint dstX0,
                                         GLint dstY0,
                                         GLint dstX1,
                                         GLint dstY1,
                                         GLbitfield mask,
                                         GLenum filter);
void APIENTRY hkglBlitFramebuffer_ThirdParty(GLint srcX0,
                                             GLint srcY0,
                                             GLint srcX1,
                                             GLint srcY1,
                                             GLint dstX0,
                                             GLint dstY0,
                                             GLint dstX1,
                                             GLint dstY1,
                                             GLbitfield mask,
                                             GLenum filter);
void APIENTRY hkglNamedFramebufferTexture(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level);

static inline void TrackNamedFramebufferTextureAttachment(GLenum attachment, GLuint texture) {
    if (attachment != GL_COLOR_ATTACHMENT0) {
        return;
    }

    const DWORD lastSwapThreadId = g_lastSwapBuffersThreadId.load(std::memory_order_acquire);
    if (lastSwapThreadId != 0 && GetCurrentThreadId() != lastSwapThreadId) {
        return;
    }

    g_lastTrackedGameFramebufferTextureId.store(texture != 0 ? texture : UINT_MAX, std::memory_order_release);
}

static inline void TrackCurrentReadFramebufferColorAttachmentTexture() {
    const DWORD lastSwapThreadId = g_lastSwapBuffersThreadId.load(std::memory_order_acquire);
    if (lastSwapThreadId != 0 && GetCurrentThreadId() != lastSwapThreadId) {
        return;
    }

    GLint attachmentType = GL_NONE;
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER,
                                          GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                          &attachmentType);
    if (attachmentType != GL_TEXTURE) {
        g_lastTrackedGameFramebufferTextureId.store(UINT_MAX, std::memory_order_release);
        return;
    }

    GLint attachmentName = 0;
    glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER,
                                          GL_COLOR_ATTACHMENT0,
                                          GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                                          &attachmentName);
    g_lastTrackedGameFramebufferTextureId.store(attachmentName > 0 ? static_cast<GLuint>(attachmentName) : UINT_MAX,
                                                std::memory_order_release);
}

static void AttemptHookGlBlitNamedFramebufferViaGlew() {
    static std::atomic<bool> s_hooked{ false };
    if (s_hooked.load(std::memory_order_acquire)) return;
    if (oglBlitNamedFramebuffer != NULL) {
        s_hooked.store(true, std::memory_order_release);
        return;
    }

    PFNGLBLITNAMEDFRAMEBUFFERPROC pFunc = glBlitNamedFramebuffer;
    if (pFunc == NULL) return;

    MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(pFunc), reinterpret_cast<void*>(&hkglBlitNamedFramebuffer),
                                reinterpret_cast<void**>(&oglBlitNamedFramebuffer));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        return;
    }
    st = MH_EnableHook(reinterpret_cast<void*>(pFunc));
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        return;
    }

    s_hooked.store(true, std::memory_order_release);
    LogCategory("init", "Successfully hooked glBlitNamedFramebuffer via GLEW");
}

static bool ShouldRetargetMinecraftBlitFramebuffer(GLint readFBO, GLint drawFBO) {
    return drawFBO == 0 && readFBO != 0 && IsVersionInRange(g_gameVersion, GameVersion(1, 21, 2), GameVersion(1, 21, 4));
}

static inline void BlitFramebufferHook_Impl(PFNGLBLITFRAMEBUFFERPROC_HOOK next,
                                            GLint srcX0,
                                            GLint srcY0,
                                            GLint srcX1,
                                            GLint srcY1,
                                            GLint dstX0,
                                            GLint dstY0,
                                            GLint dstX1,
                                            GLint dstY1,
                                            GLbitfield mask,
                                            GLenum filter) {
    if (!next) {
        return;
    }

    GLint readFBO = 0;
    GLint drawFBO = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFBO);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFBO);

    if (ShouldRetargetMinecraftBlitFramebuffer(readFBO, drawFBO)) {
        if ((mask & GL_COLOR_BUFFER_BIT) != 0) {
            TrackCurrentReadFramebufferColorAttachmentTexture();
        }

        int resolvedDstX0 = 0;
        int resolvedDstY0 = 0;
        int resolvedDstX1 = 0;
        int resolvedDstY1 = 0;
        if (ResolvePresentedGameBlitRect(resolvedDstX0, resolvedDstY0, resolvedDstX1, resolvedDstY1)) {
            next(srcX0, srcY0, srcX1, srcY1, resolvedDstX0, resolvedDstY0, resolvedDstX1, resolvedDstY1, mask, filter);
            return;
        }
    }

    if (TryObsBlitFramebufferRedirect(readFBO, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter)) {
        return;
    }

    next(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

static void AttemptHookGlBlitFramebufferViaWgl() {
    static std::atomic<bool> s_hooked{ false };
    if (s_hooked.load(std::memory_order_acquire)) return;

    HMODULE hOpenGL32 = GetModuleHandle(L"opengl32.dll");
    if (!hOpenGL32) return;

    void* pBlitFramebufferExport = reinterpret_cast<void*>(GetProcAddress(hOpenGL32, "glBlitFramebuffer"));

    typedef PROC(WINAPI* PFN_wglGetProcAddress)(LPCSTR);
    PFN_wglGetProcAddress pwglGetProcAddress =
        reinterpret_cast<PFN_wglGetProcAddress>(GetProcAddress(hOpenGL32, "wglGetProcAddress"));
    if (!pwglGetProcAddress) return;

    PROC pBlitFramebufferWGL = pwglGetProcAddress("glBlitFramebuffer");
    if (pBlitFramebufferWGL != NULL &&
        reinterpret_cast<void*>(pBlitFramebufferWGL) != reinterpret_cast<void*>(&hkglBlitFramebuffer) &&
        reinterpret_cast<void*>(pBlitFramebufferWGL) != reinterpret_cast<void*>(&hkglBlitFramebuffer_Driver) &&
        reinterpret_cast<void*>(pBlitFramebufferWGL) != pBlitFramebufferExport) {
        LogCategory("init", "Attempting glBlitFramebuffer hook via wglGetProcAddress: " +
                   std::to_string(reinterpret_cast<uintptr_t>(pBlitFramebufferWGL)));

        MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(pBlitFramebufferWGL), reinterpret_cast<void*>(&hkglBlitFramebuffer_Driver),
                                     reinterpret_cast<void**>(&g_oglBlitFramebufferDriver));
        if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
            st = MH_EnableHook(reinterpret_cast<void*>(pBlitFramebufferWGL));
            if (st == MH_OK || st == MH_ERROR_ENABLED) {
                s_hooked.store(true, std::memory_order_release);
                g_glBlitFramebufferHooked.store(true, std::memory_order_release);
                LogCategory("init", "SUCCESS: glBlitFramebuffer hooked via wglGetProcAddress");
                return;
            }
        }
    }

    PFNGLBLITFRAMEBUFFERPROC_HOOK pBlitFramebufferGLEW = glBlitFramebuffer;
    if (pBlitFramebufferGLEW != NULL &&
        reinterpret_cast<void*>(pBlitFramebufferGLEW) != reinterpret_cast<void*>(&hkglBlitFramebuffer) &&
        reinterpret_cast<void*>(pBlitFramebufferGLEW) != reinterpret_cast<void*>(&hkglBlitFramebuffer_Driver) &&
        reinterpret_cast<void*>(pBlitFramebufferGLEW) != pBlitFramebufferExport) {
        LogCategory("init", "Attempting glBlitFramebuffer hook via GLEW pointer: " +
                   std::to_string(reinterpret_cast<uintptr_t>(pBlitFramebufferGLEW)));

        MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(pBlitFramebufferGLEW), reinterpret_cast<void*>(&hkglBlitFramebuffer_Driver),
                                     reinterpret_cast<void**>(&g_oglBlitFramebufferDriver));
        if (st == MH_OK || st == MH_ERROR_ALREADY_CREATED) {
            st = MH_EnableHook(reinterpret_cast<void*>(pBlitFramebufferGLEW));
            if (st == MH_OK || st == MH_ERROR_ENABLED) {
                s_hooked.store(true, std::memory_order_release);
                g_glBlitFramebufferHooked.store(true, std::memory_order_release);
                LogCategory("init", "SUCCESS: glBlitFramebuffer hooked via GLEW pointer");
            }
        }
    }
}

static void AttemptHookGlBlitFramebufferThirdParty() {
    if (!g_graphicsHookDetected.load(std::memory_order_acquire)) {
        return;
    }

    if (g_glBlitFramebufferThirdPartyHookTarget.load(std::memory_order_acquire) != nullptr) {
        return;
    }

    HMODULE hOpenGL32 = GetModuleHandle(L"opengl32.dll");
    if (!hOpenGL32) {
        return;
    }

    typedef PROC(WINAPI* PFN_wglGetProcAddress)(LPCSTR);
    PFN_wglGetProcAddress pwglGetProcAddress =
        reinterpret_cast<PFN_wglGetProcAddress>(GetProcAddress(hOpenGL32, "wglGetProcAddress"));
    if (!pwglGetProcAddress) {
        return;
    }

    void* startAddress = reinterpret_cast<void*>(pwglGetProcAddress("glBlitFramebuffer"));
    if (!startAddress) {
        return;
    }

    void* hookTarget = ResolveBlitFramebufferThirdPartyTarget(startAddress);
    if (!hookTarget) {
        return;
    }

    if (HookChain::TryCreateAndEnableHook(hookTarget, reinterpret_cast<void*>(&hkglBlitFramebuffer_ThirdParty),
                                          reinterpret_cast<void**>(&g_oglBlitFramebufferThirdParty),
                                          "glBlitFramebuffer (third-party chain)")) {
        g_glBlitFramebufferThirdPartyHookTarget.store(hookTarget, std::memory_order_release);
        LogCategory("hookchain",
                    std::string("[glBlitFramebuffer] installed third-party hook target ") +
                        HookChain::DescribeAddressWithOwner(hookTarget));
    }
}

static void AttemptHookGlNamedFramebufferTextureViaGlew() {
    static std::atomic<bool> s_hooked{ false };
    if (s_hooked.load(std::memory_order_acquire)) return;
    if (oglNamedFramebufferTexture != NULL) {
        s_hooked.store(true, std::memory_order_release);
        return;
    }

    GLNAMEDFRAMEBUFFERTEXTUREPROC pFunc = glNamedFramebufferTexture;
    if (pFunc == NULL) return;

    MH_STATUS st = MH_CreateHook(reinterpret_cast<void*>(pFunc), reinterpret_cast<void*>(&hkglNamedFramebufferTexture),
                                 reinterpret_cast<void**>(&oglNamedFramebufferTexture));
    if (st != MH_OK && st != MH_ERROR_ALREADY_CREATED) {
        return;
    }
    st = MH_EnableHook(reinterpret_cast<void*>(pFunc));
    if (st != MH_OK && st != MH_ERROR_ENABLED) {
        return;
    }

    s_hooked.store(true, std::memory_order_release);
    LogCategory("init", "Successfully hooked glNamedFramebufferTexture via GLEW");
}

static BOOL SetCursorPosHook_Impl(SETCURSORPOSPROC next, int X, int Y) {
    if (!next) return FALSE;

    const bool guiOpen = g_showGui.load(std::memory_order_acquire);
    const CapturingState capturingState = g_capturingMousePos.load(std::memory_order_acquire);
    HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
    if (hwnd != NULL && !IsWindowInForegroundTree(hwnd)) {
        if (guiOpen || capturingState != CapturingState::NONE) {
            g_capturingMousePos.store(CapturingState::NONE, std::memory_order_release);
            g_nextMouseXY.store(std::make_pair(-1, -1), std::memory_order_release);
            return TRUE;
        }
        return next(X, Y);
    }

    if (guiOpen) { return TRUE; }
    if (g_isShuttingDown.load()) { return next(X, Y); }

    ModeViewportInfo viewport = GetCurrentModeViewport();
    if (!viewport.valid) { return next(X, Y); }

    if (capturingState == CapturingState::DISABLED) {
        // Convert viewport center (client-space) into absolute screen coordinates only when needed.
        int centerX = viewport.stretchX + viewport.stretchWidth / 2;
        int centerY = viewport.stretchY + viewport.stretchHeight / 2;
        int centerX_abs = X;
        int centerY_abs = Y;
        HWND hwnd = g_minecraftHwnd.load();
        RECT clientRectScreen{};
        if (GetWindowClientRectInScreen(hwnd, clientRectScreen)) {
            centerX_abs = clientRectScreen.left + centerX;
            centerY_abs = clientRectScreen.top + centerY;
        } else {
            RECT monRect{};
            if (GetMonitorRectForWindow(hwnd, monRect)) {
                centerX_abs = monRect.left + centerX;
                centerY_abs = monRect.top + centerY;
            }
        }

        g_nextMouseXY.store(std::make_pair(centerX_abs, centerY_abs));
        return next(X, Y);
    }

    if (capturingState == CapturingState::NORMAL) {
        auto [expectedX, expectedY] = g_nextMouseXY.load();
        if (expectedX == -1 && expectedY == -1) { return next(X, Y); }
        return next(expectedX, expectedY);
    }

    return next(X, Y);
}

BOOL WINAPI hkSetCursorPos(int X, int Y) { return SetCursorPosHook_Impl(oSetCursorPos, X, Y); }

BOOL WINAPI hkSetCursorPos_ThirdParty(int X, int Y) {
    SETCURSORPOSPROC next = g_oSetCursorPosThirdParty ? g_oSetCursorPosThirdParty : oSetCursorPos;
    return SetCursorPosHook_Impl(next, X, Y);
}

static BOOL GetClientRectHook_Impl(GETCLIENTRECTPROC next, HWND hWnd, LPRECT lpRect) {
    if (!next) {
        return FALSE;
    }

    const BOOL result = next(hWnd, lpRect);
    if (!result || lpRect == nullptr) {
        return result;
    }

    if (!g_gameVersion.valid || g_gameVersion >= GameVersion(1, 13, 0)) {
        return result;
    }

    void* caller_address = _ReturnAddress();
    if (!IsLegacyLwjglCaller(caller_address)) {
        return result;
    }

    const int actualWidth = lpRect->right - lpRect->left;
    const int actualHeight = lpRect->bottom - lpRect->top;
    int requestedWidth = 0;
    int requestedHeight = 0;
    if (!ShouldUseLegacyRequestedClientRect(hWnd, actualWidth, actualHeight, requestedWidth, requestedHeight)) {
        return result;
    }

    lpRect->left = 0;
    lpRect->top = 0;
    lpRect->right = requestedWidth;
    lpRect->bottom = requestedHeight;
    return TRUE;
}

BOOL WINAPI hkGetClientRect(HWND hWnd, LPRECT lpRect) { return GetClientRectHook_Impl(oGetClientRect, hWnd, lpRect); }

static void SyncLegacyLwjglRequestedResizeOnSwapThread(HWND hwnd) {
    if (!g_gameVersion.valid || g_gameVersion >= GameVersion(1, 13, 0)) {
        return;
    }

    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }

    int requestedWidth = 0;
    int requestedHeight = 0;
    int previousRequestedWidth = 0;
    int previousRequestedHeight = 0;
    if (!GetRecentRequestedWindowClientResizes(requestedWidth, requestedHeight, previousRequestedWidth, previousRequestedHeight)) {
        return;
    }

    if (requestedWidth <= 0 || requestedHeight <= 0) {
        return;
    }

    const HWND lastSyncedHwnd = g_lastLegacyLwjglResizeSyncHwnd.load(std::memory_order_acquire);
    const int lastSyncedWidth = g_lastLegacyLwjglResizeSyncWidth.load(std::memory_order_acquire);
    const int lastSyncedHeight = g_lastLegacyLwjglResizeSyncHeight.load(std::memory_order_acquire);
    if (lastSyncedHwnd == hwnd && lastSyncedWidth == requestedWidth && lastSyncedHeight == requestedHeight) {
        return;
    }

    if (!PostMessageW(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(requestedWidth, requestedHeight))) {
        Log("[WINDOW] Failed to post legacy LWJGL WM_SIZE resize request: " + std::to_string(requestedWidth) + "x" +
            std::to_string(requestedHeight) + ", error=" + std::to_string(GetLastError()));
        return;
    }

    g_lastLegacyLwjglResizeSyncHwnd.store(hwnd, std::memory_order_release);
    g_lastLegacyLwjglResizeSyncWidth.store(requestedWidth, std::memory_order_release);
    g_lastLegacyLwjglResizeSyncHeight.store(requestedHeight, std::memory_order_release);
}

#define GLFW_CURSOR 0x00033001
#define GLFW_CURSOR_NORMAL 0x00034001
#define GLFW_CURSOR_HIDDEN 0x00034002
#define GLFW_CURSOR_DISABLED 0x00034003

static void UpdateGuiCursorRestoreState(bool cursorVisibleAfterClose) {
    if (!g_showGui.load(std::memory_order_acquire)) { return; }

    g_wasCursorVisible.store(cursorVisibleAfterClose, std::memory_order_release);
}

static void ApplyGlfwCursorMode_Impl(GLFWSETINPUTMODE next, void* window, int value) {
    if (!next) return;

    if (value == GLFW_CURSOR_DISABLED) {
        g_capturingMousePos.store(CapturingState::DISABLED, std::memory_order_release);
        next(window, GLFW_CURSOR, value);
    } else if (value == GLFW_CURSOR_NORMAL) {
        g_capturingMousePos.store(CapturingState::NORMAL, std::memory_order_release);
        next(window, GLFW_CURSOR, value);
    } else {
        next(window, GLFW_CURSOR, value);
    }

    g_capturingMousePos.store(CapturingState::NONE, std::memory_order_release);
}

void ApplyDeferredGuiCursorModeAfterClose() {
    const int deferredMode = g_deferredGuiGlfwCursorMode.exchange(kDeferredGuiGlfwCursorModeNone, std::memory_order_acq_rel);
    if (deferredMode == kDeferredGuiGlfwCursorModeNone) { return; }

    void* window = g_lastGlfwCursorWindow.load(std::memory_order_acquire);
    GLFWSETINPUTMODE directProc = oglfwSetInputMode ? oglfwSetInputMode : g_oglfwSetInputModeThirdParty;
    if (!window || !directProc) { return; }

    ApplyGlfwCursorMode_Impl(directProc, window, deferredMode);
}

void FinalizeGuiCursorStateAfterClose() {
    if (g_forceVisibleCursorWhileGuiOpen.load(std::memory_order_acquire)) {
        if (g_gameVersion >= GameVersion(1, 13, 0) && IsCursorVisible()) {
            ShowCursor(FALSE);
        }
        g_forceVisibleCursorWhileGuiOpen.store(false, std::memory_order_release);
    }

    if (!g_wasCursorVisible.load(std::memory_order_acquire) && g_gameVersion < GameVersion(1, 13, 0)) {
        HCURSOR airCursor = g_specialCursorHandle.load(std::memory_order_acquire);
        if (airCursor != NULL) {
            SetCursor(airCursor);
        }
    }
}

static void GlfwSetInputModeHook_Impl(GLFWSETINPUTMODE next, void* window, int mode, int value) {
    if (!next) return;
    if (mode != GLFW_CURSOR) { return next(window, mode, value); }

    const bool guiOpen = g_showGui.load(std::memory_order_acquire);

    g_lastGlfwCursorWindow.store(window, std::memory_order_release);

    if (guiOpen) {
        g_deferredGuiGlfwCursorMode.store(value, std::memory_order_release);
        UpdateGuiCursorRestoreState(value == GLFW_CURSOR_NORMAL);
        g_nextMouseXY.store(std::make_pair(-1, -1), std::memory_order_release);
        g_capturingMousePos.store(CapturingState::NONE, std::memory_order_release);
        return;
    }

    ApplyGlfwCursorMode_Impl(next, window, value);
}

void hkglfwSetInputMode(void* window, int mode, int value) { GlfwSetInputModeHook_Impl(oglfwSetInputMode, window, mode, value); }

void hkglfwSetInputMode_ThirdParty(void* window, int mode, int value) {
    GLFWSETINPUTMODE next = g_oglfwSetInputModeThirdParty ? g_oglfwSetInputModeThirdParty : oglfwSetInputMode;
    GlfwSetInputModeHook_Impl(next, window, mode, value);
}

static UINT GetRawInputDataHook_Impl(GETRAWINPUTDATAPROC next, HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize,
                                    UINT cbSizeHeader) {
    if (!next) return static_cast<UINT>(-1);

    UINT result = next(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);

    g_wmMouseMoveCount.store(0);

    if (result == static_cast<UINT>(-1) || pData == nullptr || uiCommand != RID_INPUT) { return result; }

    if (g_showGui.load() || g_isShuttingDown.load()) { return result; }

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(pData);

    if (raw->header.dwType == RIM_TYPEMOUSE) {
        float sensitivityX = 1.0f;
        float sensitivityY = 1.0f;
        bool sensitivityDetermined = false;

        {
            std::lock_guard<std::mutex> lock(g_tempSensitivityMutex);
            if (g_tempSensitivityOverride.active) {
                sensitivityX = g_tempSensitivityOverride.sensitivityX;
                sensitivityY = g_tempSensitivityOverride.sensitivityY;
                sensitivityDetermined = true;
            }
        }

        if (!sensitivityDetermined) {
            const ViewportTransitionSnapshot& transitionSnap =
                g_viewportTransitionSnapshots[g_viewportTransitionSnapshotIndex.load(std::memory_order_acquire)];

            std::string modeId;
            if (transitionSnap.active) {
                modeId = transitionSnap.toModeId;
            } else {
                modeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
            }

            auto inputCfgSnap = GetConfigSnapshot();
            const ModeConfig* mode = inputCfgSnap ? GetModeFromSnapshotOrFallback(*inputCfgSnap, modeId) : nullptr;
            if (mode && mode->sensitivityOverrideEnabled) {
                if (mode->separateXYSensitivity) {
                    sensitivityX = mode->modeSensitivityX;
                    sensitivityY = mode->modeSensitivityY;
                } else {
                    sensitivityX = mode->modeSensitivity;
                    sensitivityY = mode->modeSensitivity;
                }
            } else if (inputCfgSnap) {
                sensitivityX = inputCfgSnap->mouseSensitivity;
                sensitivityY = inputCfgSnap->mouseSensitivity;
            }
        }

        if (!(raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            static float xAccum = 0.0f;
            static float yAccum = 0.0f;
            static float lastSensitivityX = 1.0f;
            static float lastSensitivityY = 1.0f;
            static bool hasLastSensitivity = false;

            const bool sensitivityChanged = !hasLastSensitivity || (std::fabs(sensitivityX - lastSensitivityX) > 0.000001f) ||
                                          (std::fabs(sensitivityY - lastSensitivityY) > 0.000001f);
            if (sensitivityChanged) {
                xAccum = 0.0f;
                yAccum = 0.0f;
                lastSensitivityX = sensitivityX;
                lastSensitivityY = sensitivityY;
                hasLastSensitivity = true;
            }

            const LONG rawX = raw->data.mouse.lLastX;
            const LONG rawY = raw->data.mouse.lLastY;

            // Prevent stale remainder from one direction delaying opposite-direction output.
            if (rawX != 0 && xAccum != 0.0f && ((rawX > 0) != (xAccum > 0.0f))) { xAccum = 0.0f; }
            if (rawY != 0 && yAccum != 0.0f && ((rawY > 0) != (yAccum > 0.0f))) { yAccum = 0.0f; }

            if (sensitivityX != 1.0f || sensitivityY != 1.0f) {
                xAccum += rawX * sensitivityX;
                yAccum += rawY * sensitivityY;

                float roundedX = std::round(xAccum);
                float roundedY = std::round(yAccum);

                if (roundedX > static_cast<float>(LONG_MAX)) roundedX = static_cast<float>(LONG_MAX);
                if (roundedX < static_cast<float>(LONG_MIN)) roundedX = static_cast<float>(LONG_MIN);
                if (roundedY > static_cast<float>(LONG_MAX)) roundedY = static_cast<float>(LONG_MAX);
                if (roundedY < static_cast<float>(LONG_MIN)) roundedY = static_cast<float>(LONG_MIN);

                LONG outputX = static_cast<LONG>(roundedX);
                LONG outputY = static_cast<LONG>(roundedY);

                xAccum -= static_cast<float>(outputX);
                yAccum -= static_cast<float>(outputY);

                raw->data.mouse.lLastX = outputX;
                raw->data.mouse.lLastY = outputY;
            } else {
                // No scaling: avoid carrying stale fractional remainder across future overrides.
                xAccum = 0.0f;
                yAccum = 0.0f;
            }
        }
    }

    return result;
}

UINT WINAPI hkGetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
    return GetRawInputDataHook_Impl(oGetRawInputData, hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
}

UINT WINAPI hkGetRawInputData_ThirdParty(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
    GETRAWINPUTDATAPROC next = g_oGetRawInputDataThirdParty ? g_oGetRawInputDataThirdParty : oGetRawInputData;
    return GetRawInputDataHook_Impl(next, hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
}

void APIENTRY hkglNamedFramebufferTexture(GLuint framebuffer, GLenum attachment, GLuint texture, GLint level) {
    (void)framebuffer;
    (void)level;

    void* caller_address = _ReturnAddress();
    if (IsDynamicMemoryCaller(caller_address)) {
        TrackNamedFramebufferTextureAttachment(attachment, texture);
    }

    if (oglNamedFramebufferTexture) {
        oglNamedFramebufferTexture(framebuffer, attachment, texture, level);
    }
}

static bool ResolvePresentedGameViewportGeometry(int& outModeWidth,
                                                 int& outModeHeight,
                                                 bool& outStretchEnabled,
                                                 int& outStretchX,
                                                 int& outStretchY,
                                                 int& outStretchWidth,
                                                 int& outStretchHeight) {
    const ViewportTransitionSnapshot& transitionSnap =
        g_viewportTransitionSnapshots[g_viewportTransitionSnapshotIndex.load(std::memory_order_acquire)];
    auto hookConfigSnap = GetConfigSnapshot();
    const bool hideAnimationsInGame = hookConfigSnap ? hookConfigSnap->hideAnimationsInGame : g_config.hideAnimationsInGame;
    const bool cutGameViewportTransition = transitionSnap.active && hideAnimationsInGame;
    const CachedModeViewport& cachedMode = g_viewportModeCache[g_viewportModeCacheIndex.load(std::memory_order_acquire)];
    int modeWidth = 0;
    int modeHeight = 0;
    bool stretchEnabled = false;
    int stretchX = 0;
    int stretchY = 0;
    int stretchWidth = 0;
    int stretchHeight = 0;

    if (transitionSnap.active && !cutGameViewportTransition) {
        modeWidth = transitionSnap.toNativeWidth;
        modeHeight = transitionSnap.toNativeHeight;
        stretchEnabled = true;
        stretchX = transitionSnap.toX;
        stretchY = transitionSnap.toY;
        stretchWidth = transitionSnap.toWidth;
        stretchHeight = transitionSnap.toHeight;
    } else if (GetLatestViewportForHook(modeWidth, modeHeight, stretchEnabled, stretchX, stretchY, stretchWidth, stretchHeight)) {
        // Use live, recalculated dimensions so WM_SIZE-driven relative/expression updates
        // are reflected immediately even before the periodic viewport cache refresh.
    } else if (cachedMode.valid) {
        modeWidth = cachedMode.width;
        modeHeight = cachedMode.height;
        stretchEnabled = cachedMode.stretchEnabled;
        stretchX = cachedMode.stretchX;
        stretchY = cachedMode.stretchY;
        stretchWidth = cachedMode.stretchWidth;
        stretchHeight = cachedMode.stretchHeight;
    } else {
        return false;
    }

    const int screenW = GetCachedWindowWidth();
    const int screenH = GetCachedWindowHeight();
    if (screenW <= 0 || screenH <= 0) { return false; }

    if (transitionSnap.active && !cutGameViewportTransition) {
        if (hideAnimationsInGame) {
            stretchX = transitionSnap.toX;
            stretchY = transitionSnap.toY;
            stretchWidth = transitionSnap.toWidth;
            stretchHeight = transitionSnap.toHeight;
        } else {
            stretchX = transitionSnap.currentX;
            stretchY = transitionSnap.currentY;
            stretchWidth = transitionSnap.currentWidth;
            stretchHeight = transitionSnap.currentHeight;
        }
    } else if (!stretchEnabled) {
        stretchX = GetCenteredAxisOffset(screenW, modeWidth);
        stretchY = GetCenteredAxisOffset(screenH, modeHeight);
        stretchWidth = modeWidth;
        stretchHeight = modeHeight;
    }

    outModeWidth = modeWidth;
    outModeHeight = modeHeight;
    outStretchEnabled = stretchEnabled;
    outStretchX = stretchX;
    outStretchY = stretchY;
    outStretchWidth = stretchWidth;
    outStretchHeight = stretchHeight;

    return true;
}

bool ResolvePresentedGameViewport(ModeViewportInfo& outViewport) {
    int modeWidth = 0;
    int modeHeight = 0;
    bool stretchEnabled = false;
    int stretchX = 0;
    int stretchY = 0;
    int stretchWidth = 0;
    int stretchHeight = 0;

    if (!ResolvePresentedGameViewportGeometry(modeWidth, modeHeight, stretchEnabled, stretchX, stretchY, stretchWidth, stretchHeight)) {
        return false;
    }

    outViewport.valid = true;
    outViewport.x = 0;
    outViewport.y = 0;
    outViewport.width = modeWidth;
    outViewport.height = modeHeight;
    outViewport.stretchEnabled = stretchEnabled;
    outViewport.stretchX = stretchX;
    outViewport.stretchY = stretchY;
    outViewport.stretchWidth = stretchWidth;
    outViewport.stretchHeight = stretchHeight;
    return true;
}

bool GetLatestGameViewportSize(int& outWidth, int& outHeight) {
    if (g_showGui.load(std::memory_order_acquire)) {
        outWidth = 0;
        outHeight = 0;
        return false;
    }

    const int width = lastViewportW.load(std::memory_order_relaxed);
    const int height = lastViewportH.load(std::memory_order_relaxed);
    if (width <= 0 || height <= 0) {
        outWidth = 0;
        outHeight = 0;
        return false;
    }

    outWidth = width;
    outHeight = height;
    return true;
}

void InvalidateLatestGameViewportSize() {
    lastViewportW.store(0, std::memory_order_relaxed);
    lastViewportH.store(0, std::memory_order_relaxed);
}

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
void SetLatestGameViewportSizeForTests(int width, int height) {
    lastViewportW.store((std::max)(0, width), std::memory_order_relaxed);
    lastViewportH.store((std::max)(0, height), std::memory_order_relaxed);
}
#endif

bool ResolvePresentedGameBlitRect(int& outDstX0, int& outDstY0, int& outDstX1, int& outDstY1) {
    int modeWidth = 0;
    int modeHeight = 0;
    bool stretchEnabled = false;
    int stretchX = 0;
    int stretchY = 0;
    int stretchWidth = 0;
    int stretchHeight = 0;

    if (!ResolvePresentedGameViewportGeometry(modeWidth, modeHeight, stretchEnabled, stretchX, stretchY, stretchWidth, stretchHeight)) {
        return false;
    }

    const int screenH = GetCachedWindowHeight();
    if (screenH <= 0) { return false; }

    outDstX0 = stretchX;
    outDstY0 = screenH - stretchY - stretchHeight;
    outDstX1 = stretchX + stretchWidth;
    outDstY1 = screenH - stretchY;
    return true;
}

void APIENTRY hkglBlitFramebuffer(GLint srcX0,
                                  GLint srcY0,
                                  GLint srcX1,
                                  GLint srcY1,
                                  GLint dstX0,
                                  GLint dstY0,
                                  GLint dstX1,
                                  GLint dstY1,
                                  GLbitfield mask,
                                  GLenum filter) {
    if (!oglBlitFramebuffer) {
        return;
    }

    BlitFramebufferHook_Impl(oglBlitFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

void APIENTRY hkglBlitFramebuffer_Driver(GLint srcX0,
                                         GLint srcY0,
                                         GLint srcX1,
                                         GLint srcY1,
                                         GLint dstX0,
                                         GLint dstY0,
                                         GLint dstX1,
                                         GLint dstY1,
                                         GLbitfield mask,
                                         GLenum filter) {
    if (!g_oglBlitFramebufferDriver) {
        return;
    }

    BlitFramebufferHook_Impl(g_oglBlitFramebufferDriver, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

void APIENTRY hkglBlitFramebuffer_ThirdParty(GLint srcX0,
                                             GLint srcY0,
                                             GLint srcX1,
                                             GLint srcY1,
                                             GLint dstX0,
                                             GLint dstY0,
                                             GLint dstX1,
                                             GLint dstY1,
                                             GLbitfield mask,
                                             GLenum filter) {
    PFNGLBLITFRAMEBUFFERPROC_HOOK next = g_oglBlitFramebufferThirdParty ? g_oglBlitFramebufferThirdParty :
                                         (g_oglBlitFramebufferDriver ? g_oglBlitFramebufferDriver : oglBlitFramebuffer);
    if (!next) {
        return;
    }

    BlitFramebufferHook_Impl(next, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

void WINAPI hkglBlitNamedFramebuffer(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                     GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
    if (drawFramebuffer != 0) {
        return oglBlitNamedFramebuffer(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask,
                                       filter);
    }

    int resolvedDstX0 = 0;
    int resolvedDstY0 = 0;
    int resolvedDstX1 = 0;
    int resolvedDstY1 = 0;
    if (ResolvePresentedGameBlitRect(resolvedDstX0, resolvedDstY0, resolvedDstX1, resolvedDstY1)) {
        return oglBlitNamedFramebuffer(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, resolvedDstX0, resolvedDstY0,
                                       resolvedDstX1, resolvedDstY1, mask, filter);
    }

    return oglBlitNamedFramebuffer(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

void AttemptAggressiveGlViewportHook() {
    int hooksCreated = 0;

    // We only install ONE additional driver-level hook target at a time.
    if (g_glViewportDriverHookTarget.load(std::memory_order_acquire) != nullptr) {
        return;
    }

    // Strategy 1 (preferred): Hook via wglGetProcAddress (driver-specific implementation)
    if (!g_glViewportHookedViaWGL.load()) {
        typedef PROC(WINAPI * PFN_wglGetProcAddress)(LPCSTR);
        HMODULE hOpenGL32 = GetModuleHandle(L"opengl32.dll");
        if (hOpenGL32) {
            PFN_wglGetProcAddress pwglGetProcAddress =
                reinterpret_cast<PFN_wglGetProcAddress>(GetProcAddress(hOpenGL32, "wglGetProcAddress"));
            if (pwglGetProcAddress) {
                PROC pGlViewportWGL = pwglGetProcAddress("glViewport");
                if (pGlViewportWGL != NULL && reinterpret_cast<void*>(pGlViewportWGL) != reinterpret_cast<void*>(&hkglViewport) &&
                    reinterpret_cast<void*>(pGlViewportWGL) != reinterpret_cast<void*>(&hkglViewport_Driver) &&
                    reinterpret_cast<void*>(pGlViewportWGL) != reinterpret_cast<void*>(oglViewport)) {
                    Log("Attempting glViewport hook via wglGetProcAddress: " + std::to_string(reinterpret_cast<uintptr_t>(pGlViewportWGL)));
                    GLVIEWPORTPROC pViewportFunc = reinterpret_cast<GLVIEWPORTPROC>(pGlViewportWGL);
                    if (HookChain::TryCreateAndEnableHook(reinterpret_cast<void*>(pViewportFunc), reinterpret_cast<void*>(&hkglViewport_Driver),
                                               reinterpret_cast<void**>(&g_oglViewportDriver), "glViewport (wglGetProcAddress)")) {
                        g_glViewportHookedViaWGL.store(true);
                        g_glViewportDriverHookTarget.store(reinterpret_cast<void*>(pViewportFunc), std::memory_order_release);
                        hooksCreated++;
                        Log("SUCCESS: glViewport hooked via wglGetProcAddress (driver target)");
                    }
                }
            }
        }
    }

    if (g_glViewportDriverHookTarget.load(std::memory_order_acquire) == nullptr && !g_glViewportHookedViaGLEW.load()) {
        GLVIEWPORTPROC pGlViewportGLEW = glViewport;
        if (pGlViewportGLEW != NULL && reinterpret_cast<void*>(pGlViewportGLEW) != reinterpret_cast<void*>(&hkglViewport) &&
            reinterpret_cast<void*>(pGlViewportGLEW) != reinterpret_cast<void*>(&hkglViewport_Driver) &&
            reinterpret_cast<void*>(pGlViewportGLEW) != reinterpret_cast<void*>(oglViewport)) {
            Log("Attempting glViewport hook via GLEW pointer: " + std::to_string(reinterpret_cast<uintptr_t>(pGlViewportGLEW)));
            if (HookChain::TryCreateAndEnableHook(reinterpret_cast<void*>(pGlViewportGLEW), reinterpret_cast<void*>(&hkglViewport_Driver),
                                       reinterpret_cast<void**>(&g_oglViewportDriver), "glViewport (GLEW pointer)")) {
                g_glViewportHookedViaGLEW.store(true);
                g_glViewportDriverHookTarget.store(reinterpret_cast<void*>(pGlViewportGLEW), std::memory_order_release);
                hooksCreated++;
                Log("SUCCESS: glViewport hooked via GLEW pointer (driver target)");
            }
        }
    }

    g_glViewportHookCount.fetch_add(hooksCreated);
    Log("Aggressive glViewport hooking complete. Total additional hooks created: " + std::to_string(hooksCreated));
    Log("Total glViewport hook count: " + std::to_string(g_glViewportHookCount.load()));
}

static void AttemptHookGlBindTextureViaWgl() {
    static std::atomic<bool> s_hooked{ false };
    if (s_hooked.load(std::memory_order_acquire)) return;

    HMODULE hOpenGL32 = GetModuleHandle(L"opengl32.dll");
    if (!hOpenGL32) return;

    typedef PROC(WINAPI* PFN_wglGetProcAddress)(LPCSTR);
    PFN_wglGetProcAddress pwglGetProcAddress =
        reinterpret_cast<PFN_wglGetProcAddress>(GetProcAddress(hOpenGL32, "wglGetProcAddress"));
    if (!pwglGetProcAddress) return;

    PROC pBindTexWGL = pwglGetProcAddress("glBindTexture");
    if (pBindTexWGL != NULL &&
        reinterpret_cast<void*>(pBindTexWGL) != reinterpret_cast<void*>(&hkglBindTexture) &&
        reinterpret_cast<void*>(pBindTexWGL) != reinterpret_cast<void*>(&hkglBindTexture_Driver) &&
        reinterpret_cast<void*>(pBindTexWGL) != reinterpret_cast<void*>(oglBindTexture)) {
        LogCategory("init", "Attempting glBindTexture hook via wglGetProcAddress: " +
                   std::to_string(reinterpret_cast<uintptr_t>(pBindTexWGL)));
        if (HookChain::TryCreateAndEnableHook(reinterpret_cast<void*>(pBindTexWGL),
                                              reinterpret_cast<void*>(&hkglBindTexture_Driver),
                                              reinterpret_cast<void**>(&g_oglBindTextureDriver),
                                              "glBindTexture (wglGetProcAddress)")) {
            s_hooked.store(true, std::memory_order_release);
            LogCategory("init", "SUCCESS: glBindTexture hooked via wglGetProcAddress (driver target)");
        }
    }
}

static void AttemptHookGlBindFramebufferViaWgl() {
    static std::atomic<bool> s_hooked{ false };
    if (s_hooked.load(std::memory_order_acquire)) return;

    HMODULE hOpenGL32 = GetModuleHandle(L"opengl32.dll");
    if (!hOpenGL32) return;

    typedef PROC(WINAPI* PFN_wglGetProcAddress)(LPCSTR);
    PFN_wglGetProcAddress pwglGetProcAddress =
        reinterpret_cast<PFN_wglGetProcAddress>(GetProcAddress(hOpenGL32, "wglGetProcAddress"));
    if (!pwglGetProcAddress) return;

    PROC pBindFramebufferWGL = pwglGetProcAddress("glBindFramebuffer");
    if (pBindFramebufferWGL != NULL &&
        reinterpret_cast<void*>(pBindFramebufferWGL) != reinterpret_cast<void*>(&hkglBindFramebuffer) &&
        reinterpret_cast<void*>(pBindFramebufferWGL) != reinterpret_cast<void*>(&hkglBindFramebuffer_Driver) &&
        reinterpret_cast<void*>(pBindFramebufferWGL) != reinterpret_cast<void*>(oglBindFramebuffer)) {
        LogCategory("init", "Attempting glBindFramebuffer hook via wglGetProcAddress: " +
                   std::to_string(reinterpret_cast<uintptr_t>(pBindFramebufferWGL)));
        if (HookChain::TryCreateAndEnableHook(reinterpret_cast<void*>(pBindFramebufferWGL),
                                              reinterpret_cast<void*>(&hkglBindFramebuffer_Driver),
                                              reinterpret_cast<void**>(&g_oglBindFramebufferDriver),
                                              "glBindFramebuffer (wglGetProcAddress)")) {
            s_hooked.store(true, std::memory_order_release);
            LogCategory("init", "SUCCESS: glBindFramebuffer hooked via wglGetProcAddress (driver target)");
            return;
        }
    }

    GLBINDFRAMEBUFFERPROC pBindFramebufferGLEW = glBindFramebuffer;
    if (pBindFramebufferGLEW != NULL &&
        reinterpret_cast<void*>(pBindFramebufferGLEW) != reinterpret_cast<void*>(&hkglBindFramebuffer) &&
        reinterpret_cast<void*>(pBindFramebufferGLEW) != reinterpret_cast<void*>(&hkglBindFramebuffer_Driver) &&
        reinterpret_cast<void*>(pBindFramebufferGLEW) != reinterpret_cast<void*>(oglBindFramebuffer)) {
        LogCategory("init", "Attempting glBindFramebuffer hook via GLEW pointer: " +
                   std::to_string(reinterpret_cast<uintptr_t>(pBindFramebufferGLEW)));
        if (HookChain::TryCreateAndEnableHook(reinterpret_cast<void*>(pBindFramebufferGLEW),
                                              reinterpret_cast<void*>(&hkglBindFramebuffer_Driver),
                                              reinterpret_cast<void**>(&g_oglBindFramebufferDriver),
                                              "glBindFramebuffer (GLEW pointer)")) {
            s_hooked.store(true, std::memory_order_release);
            LogCategory("init", "SUCCESS: glBindFramebuffer hooked via GLEW pointer (driver target)");
        }
    }
}

static BOOL SwapBuffersHook_Impl(WGLSWAPBUFFERS next, HDC hDc) {
    if (!next) return FALSE;

    thread_local int s_swapBuffersHookDepth = 0;
    struct SwapBuffersHookDepthScope {
        int& depth;
        explicit SwapBuffersHookDepthScope(int& value) : depth(value) { ++depth; }
        ~SwapBuffersHookDepthScope() { --depth; }
    } depthScope(s_swapBuffersHookDepth);
    if (s_swapBuffersHookDepth > 1) {
        return next(hDc);
    }

    auto startTime = std::chrono::high_resolution_clock::now();
    _set_se_translator(SEHTranslator);

    const GLuint trackedFramebufferTextureId = g_lastTrackedGameFramebufferTextureId.load(std::memory_order_acquire);
    const GLuint trackedGameTextureId = (trackedFramebufferTextureId != UINT_MAX)
                                            ? trackedFramebufferTextureId
                                            : g_lastTrackedGameTextureBindId.load(std::memory_order_acquire);
    const DWORD currentSwapThreadId = GetCurrentThreadId();
    const DWORD previousSwapThreadId = g_lastSwapBuffersThreadId.exchange(currentSwapThreadId, std::memory_order_acq_rel);
    if (previousSwapThreadId != 0 && previousSwapThreadId != currentSwapThreadId) {
        InvalidateTrackedGameTextureId(false);
    }

    {
        PROFILE_SCOPE_CAT("Calculate Game Texture ID", "SwapBuffers");
        const GLuint cachedGameTextureId = g_cachedGameTextureId.load(std::memory_order_acquire);
        if (trackedGameTextureId != UINT_MAX && cachedGameTextureId != trackedGameTextureId) {
            g_cachedGameTextureId.store(trackedGameTextureId, std::memory_order_release);
        }
    }

    try {
        if (!g_glewLoaded) {
            PROFILE_SCOPE_CAT("GLEW Initialization", "SwapBuffers");
            glewExperimental = GL_TRUE;
            if (glewInit() == GLEW_OK) {
                LogCategory("init", "[RENDER] GLEW Initialized successfully.");
                g_glewLoaded = true;

                g_welcomeToastVisible.store(true);

                CursorTextures::LoadCursorTextures();

                if (wglGetCurrentContext()) {
                    AttemptHookGlBlitFramebufferViaWgl();
                    StartObsHookThread();
                }

                AttemptAggressiveGlViewportHook();

                AttemptHookGlBindTextureViaWgl();

                AttemptHookGlBindFramebufferViaWgl();

                AttemptHookGlNamedFramebufferTextureViaGlew();

                AttemptHookGlBlitNamedFramebufferViaGlew();

            } else {
                Log("[RENDER] ERROR: Failed to initialize GLEW.");
                return next(hDc);
            }
        }
        if (g_isShuttingDown.load()) { return next(hDc); }

        // Start logic thread if not already running (handles OBS detection, hotkey resets, etc.)
        if (!g_logicThreadRunning.load() && g_configLoaded.load()) { StartLogicThread(); }

        // Early exit if config hasn't been loaded yet (prevents race conditions during startup)
        if (!g_configLoaded.load()) { return next(hDc); }

        auto frameCfgSnap = GetConfigSnapshot();
        if (!frameCfgSnap) { return next(hDc); }
        const Config& frameCfg = *frameCfgSnap;

        HWND hwnd = WindowFromDC(hDc);
        if (!hwnd) { return next(hDc); }
        HWND previousHwnd = g_minecraftHwnd.load();
        if (hwnd != previousHwnd) {
            g_minecraftHwnd.store(hwnd);
            ApplyConfineCursorToGameWindow();
        }

        SyncVirtualCameraRuntimeState(frameCfg.debug.virtualCameraEnabled);

        // This copy is expensive: it blits the full game texture + inserts fences + flushes.
        {
            const bool needCaptureForMirrors = (g_activeMirrorCaptureCount.load(std::memory_order_acquire) > 0);
            const bool needCaptureForEyeZoom = g_showEyeZoom.load(std::memory_order_relaxed) ||
                                               g_isTransitioningFromEyeZoom.load(std::memory_order_relaxed);
            const bool needCaptureForObsOrVc = g_graphicsHookDetected.load(std::memory_order_acquire) || IsVirtualCameraActive();

            const bool needCapture = needCaptureForMirrors || needCaptureForObsOrVc;
            const bool needAsyncCaptureCopy = needCaptureForObsOrVc;
            if (needAsyncCaptureCopy) {
                static auto s_lastMirrorOnlyCaptureSubmit = std::chrono::steady_clock::time_point{};
                static int s_lastMirrorOnlyW = 0;
                static int s_lastMirrorOnlyH = 0;
                bool allowCaptureThisFrame = true;

                GLuint gameTexture = g_cachedGameTextureId.load(std::memory_order_acquire);
                if (gameTexture != UINT_MAX) {
                    ModeViewportInfo viewport = GetCurrentModeViewport();
                    int textureWidth = 0;
                    int textureHeight = 0;
                    const bool hasTextureSize = GetTexture2DLevel0Size(gameTexture, textureWidth, textureHeight);
                    const int captureWidth = hasTextureSize ? textureWidth : (viewport.valid ? viewport.width : 0);
                    const int captureHeight = hasTextureSize ? textureHeight : (viewport.valid ? viewport.height : 0);

                    if (captureWidth > 0 && captureHeight > 0) {

                        if (needCaptureForMirrors && !needCaptureForEyeZoom && !needCaptureForObsOrVc) {
                            const int maxMirrorFps = g_activeMirrorCaptureMaxFps.load(std::memory_order_acquire);
                            if (maxMirrorFps > 0 && !IsMirrorRealtimeFps(maxMirrorFps)) {
                                const auto now = std::chrono::steady_clock::now();
                                const double intervalMsD = 1000.0 / static_cast<double>((std::max)(1, maxMirrorFps));
                                const auto interval = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double, std::milli>(intervalMsD));

                                const bool dimsChanged = (captureWidth != s_lastMirrorOnlyW) || (captureHeight != s_lastMirrorOnlyH);

                                if (!dimsChanged && s_lastMirrorOnlyCaptureSubmit.time_since_epoch().count() != 0) {
                                    if ((now - s_lastMirrorOnlyCaptureSubmit) < interval) {
                                        allowCaptureThisFrame = false;
                                    }
                                }

                                if (allowCaptureThisFrame) {
                                    s_lastMirrorOnlyCaptureSubmit = now;
                                    s_lastMirrorOnlyW = captureWidth;
                                    s_lastMirrorOnlyH = captureHeight;
                                }
                            }
                        }

                        // SubmitFrameCapture already inserts its own fences and flushes after them;
                        // avoid an extra glFlush here (it can reduce FPS by forcing more driver work per frame).
                        if (allowCaptureThisFrame) {
                            EnsureCaptureTextureInitialized(captureWidth, captureHeight);
                            SubmitFrameCapture(gameTexture, captureWidth, captureHeight);
                        }
                    }
                }
            }
        }

        bool shouldCheckSubclass = (g_gameVersion < GameVersion(1, 13, 0)) || (g_originalWndProc == NULL);

        if (shouldCheckSubclass && hwnd != NULL) {
            PROFILE_SCOPE_CAT("Window Subclassing", "SwapBuffers");
            SubclassGameWindow(hwnd);
        }

        {
            bool showTextureGrid = frameCfg.debug.showTextureGrid;
            ModeViewportInfo viewport = GetCurrentModeViewport();
            // Store texture grid state so the render thread can start an ImGui frame for text labels
            g_showTextureGrid.store(showTextureGrid, std::memory_order_relaxed);
            g_textureGridModeWidth.store(viewport.width, std::memory_order_relaxed);
            g_textureGridModeHeight.store(viewport.height, std::memory_order_relaxed);
            if (showTextureGrid && g_glInitialized.load(std::memory_order_acquire) && g_solidColorProgram != 0) {
                PROFILE_SCOPE_CAT("Texture Grid Overlay", "Debug");
                RenderTextureGridOverlay(true, viewport.width, viewport.height);
            }
        }

        const int fullW = GetCachedWindowWidth(), fullH = GetCachedWindowHeight();

        int windowWidth = 0, windowHeight = 0;
        {
            RECT rect;
            if (GetClientRect(hwnd, &rect)) {
                windowWidth = rect.right - rect.left;
                windowHeight = rect.bottom - rect.top;
            }
        }

        constexpr int kFullscreenTolPx = 1;
        const bool hasWindowClientSize = (windowWidth > 0 && windowHeight > 0);
        const bool isWindowedPresentation =
            hasWindowClientSize && (windowWidth < (fullW - kFullscreenTolPx) || windowHeight < (fullH - kFullscreenTolPx));

        if (isWindowedPresentation) {
            bool isPre113 = (g_gameVersion < GameVersion(1, 13, 0));
            if (isPre113) {
                ModeViewportInfo viewport = GetCurrentModeViewport();
                int offsetX = 0;
                int offsetY = 0;
                int contentW = windowWidth;
                int contentH = windowHeight;

                if (viewport.valid && viewport.stretchWidth > 0 && viewport.stretchHeight > 0) {
                    offsetX = viewport.stretchX;
                    offsetY = viewport.stretchY;
                    contentW = viewport.stretchWidth;
                    contentH = viewport.stretchHeight;
                } else {
                    offsetX = GetCenteredAxisOffset(fullW, windowWidth);
                    offsetY = GetCenteredAxisOffset(fullH, windowHeight);
                }

                g_obsPre113Windowed.store(true, std::memory_order_release);
                g_obsPre113OffsetX.store(offsetX, std::memory_order_release);
                g_obsPre113OffsetY.store(offsetY, std::memory_order_release);
                g_obsPre113ContentW.store(contentW, std::memory_order_release);
                g_obsPre113ContentH.store(contentH, std::memory_order_release);
            } else {
                g_obsPre113Windowed.store(false, std::memory_order_release);
            }
        } else {
            g_obsPre113Windowed.store(false, std::memory_order_release);
        }

        const bool shouldUseObsOverride = g_graphicsHookDetected.load(std::memory_order_acquire);
        if (shouldUseObsOverride) {
            AttemptHookGlBlitFramebufferThirdParty();
            StartObsHookThread();
            EnableObsOverride();
        } else {
            ClearObsOverride();
        }


        if (g_configLoadFailed.load()) {
            Log("Configuration load failed");
            HandleConfigLoadFailed(hDc, next);
            return next(hDc);
        }

        // Lock-free read of current mode ID from double-buffer
        std::string desiredModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];

        // Lock-free read of last frame mode ID from double-buffer
        std::string lastFrameModeIdCopy = g_lastFrameModeIdBuffers[g_lastFrameModeIdIndex.load(std::memory_order_acquire)];

        if (IsModeTransitionActive()) {
            g_isTransitioningMode = true;
        } else if (lastFrameModeIdCopy != desiredModeId) {
            PROFILE_SCOPE_CAT("Mode Transition Complete", "SwapBuffers");
            g_isTransitioningMode = true;
            Log("Mode transition detected (no animation): " + lastFrameModeIdCopy + " -> " + desiredModeId);

            int modeWidth = 0, modeHeight = 0;
            bool modeValid = false;
            {
                const ModeConfig* newMode = GetModeFromSnapshotOrFallback(frameCfg, desiredModeId);
                if (newMode) {
                    modeWidth = newMode->width;
                    modeHeight = newMode->height;
                    modeValid = true;
                }
            }
            if (modeValid) { RequestWindowClientResize(hwnd, modeWidth, modeHeight, "swapbuffers:mode_transition_complete"); }
        }

        // Note: Video player update is now done in render_thread

        std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];

        bool showPerformanceOverlay = frameCfg.debug.showPerformanceOverlay;
        bool showProfiler = frameCfg.debug.showProfiler;

        Profiler::GetInstance().SetEnabled(showProfiler);
        if (showProfiler) { Profiler::GetInstance().MarkAsRenderThread(); }

        ModeConfig modeToRenderCopy;
        bool modeFound = false;
        {
            const ModeConfig* tempMode = GetModeFromSnapshotOrFallback(frameCfg, desiredModeId);
            if (!tempMode && g_isTransitioningMode) {
                tempMode = GetModeFromSnapshotOrFallback(frameCfg, lastFrameModeIdCopy);
            }
            if (tempMode) {
                modeToRenderCopy = *tempMode;
                modeFound = true;
            }
        }

        if (!modeFound) {
            Log("ERROR: Could not find mode to render, aborting frame");
            return next(hDc);
        }

        bool isEyeZoom = modeToRenderCopy.id == "EyeZoom";
        bool shouldRenderGui = g_showGui.load();

        bool isTransitioningFromEyeZoom = false;
        int eyeZoomAnimatedViewportX = -1;

        if (IsModeTransitionActive()) {
            ModeTransitionState eyeZoomTransitionState = GetModeTransitionState();
            std::string fromModeId = eyeZoomTransitionState.fromModeId;
            const bool slideAnimationsEnabled = eyeZoomTransitionState.gameTransition == GameTransitionType::Bounce;
            auto interpolateTransitionX = [&]() {
                return static_cast<int>(eyeZoomTransitionState.fromX +
                                        (eyeZoomTransitionState.targetX - eyeZoomTransitionState.fromX) *
                                            eyeZoomTransitionState.moveProgress);
            };

            if (slideAnimationsEnabled && !isEyeZoom && fromModeId == "EyeZoom") {
                isTransitioningFromEyeZoom = true;
                eyeZoomAnimatedViewportX = interpolateTransitionX();
            } else if (slideAnimationsEnabled && isEyeZoom && fromModeId != "EyeZoom") {
                eyeZoomAnimatedViewportX = eyeZoomTransitionState.x;
            }
        }

        // Set global GUI state for render thread to pick up
        g_shouldRenderGui.store(shouldRenderGui, std::memory_order_relaxed);
        g_showPerformanceOverlay.store(showPerformanceOverlay, std::memory_order_relaxed);
        g_showProfiler.store(showProfiler, std::memory_order_relaxed);
        bool hideAnimOnScreenEyeZoom = frameCfg.hideAnimationsInGame;
        bool showEyeZoomOnScreen = isEyeZoom || (isTransitioningFromEyeZoom && !hideAnimOnScreenEyeZoom);
        g_showEyeZoom.store(showEyeZoomOnScreen, std::memory_order_relaxed);
        g_eyeZoomFadeOpacity.store(1.0f, std::memory_order_relaxed);
        g_eyeZoomAnimatedViewportX.store(eyeZoomAnimatedViewportX, std::memory_order_relaxed);
        // Release ensures all preceding EyeZoom stores are visible when the reader acquires this value
        g_isTransitioningFromEyeZoom.store(isTransitioningFromEyeZoom, std::memory_order_release);

        if (!g_glInitialized.load(std::memory_order_acquire)) {
            PROFILE_SCOPE_CAT("GPU Resource Init Check", "SwapBuffers");
            Log("[RENDER] Conditions met for GPU resource initialization.");
            InitializeGPUResources();

            if (!g_glInitialized.load(std::memory_order_acquire)) {
                Log("FATAL: GPU resource initialization failed. Aborting custom render for this frame.");
                return next(hDc);
            }
        }

        // Note: Game state reset (wall/title/waiting) is now handled by logic_thread

        GLState s;
        {
            PROFILE_SCOPE_CAT("OpenGL State Backup", "SwapBuffers");
            SaveGLState(&s);
        }

        {
            PROFILE_SCOPE_CAT("Texture Cleanup", "SwapBuffers");
            if (g_hasTexturesToDelete.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> lock(g_texturesToDeleteMutex);
                if (!g_texturesToDelete.empty()) {
                    glDeleteTextures((GLsizei)g_texturesToDelete.size(), g_texturesToDelete.data());
                    g_texturesToDelete.clear();
                }
                g_hasTexturesToDelete.store(false, std::memory_order_release);
            }
        }

        if (g_pendingImageLoad) {
            PROFILE_SCOPE_CAT("Pending Image Load", "SwapBuffers");
            LoadAllImages();
            g_allImagesLoaded = true;
            g_pendingImageLoad = false;
        }

        {
            PROFILE_SCOPE_CAT("Decoded Image Uploads", "SwapBuffers");
            ProcessPendingDecodedImages();
        }

        int current_gameW = modeToRenderCopy.width;
        int current_gameH = modeToRenderCopy.height;

        g_obsCaptureReady.store(false);


        bool hideAnimOnScreen = frameCfg.hideAnimationsInGame && IsModeTransitionActive();
        {
            PROFILE_SCOPE_CAT("Normal Mode Handling", "Rendering");
            RenderMode(&modeToRenderCopy, s, current_gameW, current_gameH, hideAnimOnScreen, false);
        }

        // All ImGui rendering is handled by render thread (via FrameRenderRequest ImGui state fields)
        // Screenshot handling stays on main thread since it needs direct backbuffer access
        if (g_screenshotRequested.exchange(false)) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s.read_fb);
            ScreenshotToClipboard(fullW, fullH);
        }

        // Render fake cursor overlay if enabled (MUST be after RestoreGLState)
        {
            bool fakeCursorEnabled = frameCfg.debug.fakeCursor;
            if (fakeCursorEnabled) {
                PROFILE_SCOPE_CAT("Fake Cursor Rendering", "Rendering");
                if (IsCursorVisible()) { RenderFakeCursor(hwnd, windowWidth, windowHeight); }
            }
        }

        const bool shouldRenderObsHookFrame =
            g_graphicsHookDetected.load(std::memory_order_acquire) && ShouldUpdateObsTextureNow();
        const bool shouldRenderVirtualCameraFrame = IsVirtualCameraActive() && ShouldCaptureVirtualCameraFrame();
        const bool shouldRenderSharedObsFrame = shouldRenderObsHookFrame || shouldRenderVirtualCameraFrame;
        if (shouldRenderSharedObsFrame) {
            PROFILE_SCOPE_CAT("Capture Shared OBS/Virtual Camera Frame", "OBS");
            RenderSameThreadObsFrame(&modeToRenderCopy, s, current_gameW, current_gameH, false);
        }
        if (shouldRenderVirtualCameraFrame) {
            PROFILE_SCOPE_CAT("Capture Virtual Camera Frame", "VirtualCamera");
            CaptureSameThreadVirtualCameraFrame();
        }

        {
            PROFILE_SCOPE_CAT("OpenGL State Restore", "SwapBuffers");
            RestoreGLState(s);
        }

        Profiler::GetInstance().EndFrame();

        // Update last frame mode ID using lock-free double-buffer
        // We're the only writer on this thread, so no lock needed - just atomic swap
        {
            int nextIndex = 1 - g_lastFrameModeIdIndex.load(std::memory_order_relaxed);
            g_lastFrameModeIdBuffers[nextIndex] = desiredModeId;
            g_lastFrameModeIdIndex.store(nextIndex, std::memory_order_release);
            g_lastFrameModeId = desiredModeId; // Keep legacy variable in sync (no lock needed - single writer)
        }

        g_isTransitioningMode = false;

        int targetFPS = 0;
        { targetFPS = frameCfg.fpsLimit; }

        if (targetFPS > 0 && g_highResTimer) {
            PROFILE_SCOPE_CAT("FPS Limit Sleep", "Timing");

            const double targetFrameTimeUs = 1000000.0 / targetFPS;
            const bool isHighFPS = targetFPS > 500;

            std::lock_guard<std::mutex> lock(g_fpsLimitMutex);

            auto targetTime = g_lastFrameEndTime + std::chrono::microseconds(static_cast<long long>(targetFrameTimeUs));
            auto now = std::chrono::high_resolution_clock::now();

            if (now < targetTime) {
                auto timeToWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(targetTime - now).count();

                if (isHighFPS) {
                    if (timeToWaitUs > 1000) {
                        LARGE_INTEGER dueTime;
                        dueTime.QuadPart = -static_cast<LONGLONG>(timeToWaitUs);

                        if (SetWaitableTimer(g_highResTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                            WaitForSingleObject(g_highResTimer, 1000);
                        }
                    }
                } else {
                    if (timeToWaitUs > 10) {
                        LARGE_INTEGER dueTime;
                        dueTime.QuadPart = -static_cast<LONGLONG>(timeToWaitUs * 10LL);

                        if (SetWaitableTimer(g_highResTimer, &dueTime, 0, NULL, NULL, FALSE)) { WaitForSingleObject(g_highResTimer, 1000); }
                    }
                }

                g_lastFrameEndTime = targetTime;
            } else {
                g_lastFrameEndTime = now;
            }
        }

        if (IsModeTransitionActive()) {
            PROFILE_SCOPE_CAT("Mode Transition Animation", "SwapBuffers");
            UpdateModeTransition();
        }

        if (frameCfg.debug.delayRenderingUntilFinished) { glFinish(); }

        SyncLegacyLwjglRequestedResizeOnSwapThread(hwnd);

        auto swapStartTime = std::chrono::high_resolution_clock::now();
        BOOL result = next(hDc);

        auto swapEndTime = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> swapDuration = swapEndTime - swapStartTime;
        g_originalFrameTimeMs = swapDuration.count();

        std::chrono::duration<double, std::milli> fp_ms = swapStartTime - startTime;
        g_lastFrameTimeMs = fp_ms.count();

        // Update last frame mode ID for next frame's viewport calculations (lock-free)
        {
            int nextIndex = 1 - g_lastFrameModeIdIndex.load(std::memory_order_relaxed);
            g_lastFrameModeIdBuffers[nextIndex] = desiredModeId;
            g_lastFrameModeIdIndex.store(nextIndex, std::memory_order_release);
            g_lastFrameModeId = desiredModeId;
        }

        return result;
    } catch (const SE_Exception& e) {
        LogException("hkwglSwapBuffers (SEH)", e.getCode(), e.getInfo());
        return next(hDc);
    } catch (const std::exception& e) {
        LogException("hkwglSwapBuffers", e);
        return next(hDc);
    } catch (...) {
        Log("FATAL UNKNOWN EXCEPTION in hkwglSwapBuffers!");
        return next(hDc);
    }
}

BOOL WINAPI hkwglSwapBuffers(HDC hDc) { return SwapBuffersHook_Impl(owglSwapBuffers, hDc); }

BOOL WINAPI hkwglSwapBuffers_ThirdParty(HDC hDc) {
    void* installedHookTarget = g_wglSwapBuffersThirdPartyHookTarget.load(std::memory_order_acquire);
    if (installedHookTarget == reinterpret_cast<void*>(&hkwglSwapBuffers)) {
        WGLSWAPBUFFERS next = (g_owglSwapBuffersThirdParty && g_owglSwapBuffersThirdParty != reinterpret_cast<WGLSWAPBUFFERS>(&hkwglSwapBuffers_ThirdParty))
                                  ? g_owglSwapBuffersThirdParty
                                  : owglSwapBuffers;
        return next ? next(hDc) : FALSE;
    }

    void* caller_address = _ReturnAddress();
    if (IsDisallowedThirdPartySwapCaller(caller_address)) {
        WGLSWAPBUFFERS next = (g_owglSwapBuffersThirdParty && g_owglSwapBuffersThirdParty != reinterpret_cast<WGLSWAPBUFFERS>(&hkwglSwapBuffers_ThirdParty))
                                  ? g_owglSwapBuffersThirdParty
                                  : owglSwapBuffers;
        return next ? next(hDc) : FALSE;
    }

    WGLSWAPBUFFERS next = g_owglSwapBuffersThirdParty ? g_owglSwapBuffersThirdParty : owglSwapBuffers;
    return SwapBuffersHook_Impl(next, hDc);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)DllMain,
                           &g_hModule);

        InstallGlobalExceptionHandlers();

        LogCategory("init", "========================================");
        LogCategory("init", "=== Toolscreen INITIALIZATION START ===");
        LogCategory("init", "========================================");
        PrintVersionToStdout();

        // Create high-resolution waitable timer for FPS limiting (Windows 10 1803+)
        g_highResTimer = CreateWaitableTimerExW(NULL,
                                                NULL,
                                                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                TIMER_ALL_ACCESS
        );
        if (g_highResTimer) {
            LogCategory("init", "High-resolution waitable timer created successfully for FPS limiting.");
        } else {
            Log("Warning: Failed to create high-resolution waitable timer. FPS limiting may be less precise.");
        }

        g_toolscreenPath = GetToolscreenPath();
        if (!g_toolscreenPath.empty()) {
            std::wstring logsDir = g_toolscreenPath + L"\\logs";
            std::wstring latestLogPath = logsDir + L"\\latest.log";

            if (GetFileAttributesW(latestLogPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                HANDLE hFile =
                    CreateFileW(latestLogPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile != INVALID_HANDLE_VALUE) {
                    FILETIME lastWriteTime;
                    if (GetFileTime(hFile, NULL, NULL, &lastWriteTime)) {
                        FILETIME localFileTime;
                        FileTimeToLocalFileTime(&lastWriteTime, &localFileTime);
                        SYSTEMTIME st;
                        FileTimeToSystemTime(&localFileTime, &st);

                        WCHAR timestamp[32];
                        swprintf_s(timestamp, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

                        std::wstring archivedLogPath = logsDir + L"\\" + timestamp + L".log";

                        CloseHandle(hFile);

                        if (GetFileAttributesW(archivedLogPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                            for (int counter = 1; counter < 100; counter++) {
                                std::wstring altPath = logsDir + L"\\" + timestamp + L"_" + std::to_wstring(counter) + L".log";
                                if (GetFileAttributesW(altPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                                    archivedLogPath = altPath;
                                    break;
                                }
                            }
                        }

                        if (!MoveFileW(latestLogPath.c_str(), archivedLogPath.c_str())) {
                            Log("WARNING: Could not rename old log to " + WideToUtf8(archivedLogPath) +
                                ", error code: " + std::to_string(GetLastError()));
                        } else {
                            // Compress the archived log to .gz on a background thread
                            // so we don't block DLL initialization
                            std::wstring archiveSrc = archivedLogPath;
                            std::thread([archiveSrc]() {
                                std::wstring gzPath = archiveSrc + L".gz";
                                if (CompressFileToGzip(archiveSrc, gzPath)) {
                                    DeleteFileW(archiveSrc.c_str());
                                }
                            }).detach();
                        }
                    } else {
                        CloseHandle(hFile);
                    }
                }
            }

            if (AcquireLatestLogSession(logsDir, g_logSession)) {
                {
                    std::lock_guard<std::mutex> lock(g_logFileMutex);
                    logFile.open(std::filesystem::path(g_logSession.logFilePath), std::ios_base::out | std::ios_base::trunc);
                    if (logFile.is_open()) {
                        logFile << BuildLogSessionHeader(g_logSession);
                        logFile.flush();
                    }
                }

                if (logFile.is_open()) {
                    StartLogThread();
                } else {
                    ReleaseLatestLogSession(g_logSession);
                }
            }

            g_modeFilePath = g_toolscreenPath + L"\\mode.txt";
        }
        LogCategory("init", "--- DLL instance attached ---");
        LogVersionInfo();
        if (g_toolscreenPath.empty()) { Log("FATAL: Could not get toolscreen directory."); }
        
        StartSupportersFetch();

        g_gameVersion = GetGameVersionFromCommandLine();
        GameVersion minVersion(1, 16, 1);
        GameVersion maxVersion(1, 18, 2);

        if (g_gameVersion.valid) {
            bool inRange = IsVersionInRange(g_gameVersion, minVersion, maxVersion);

            std::ostringstream oss;
            oss << "Game version " << g_gameVersion.major << "." << g_gameVersion.minor << "." << g_gameVersion.patch;
            if (inRange) {
                oss << " is in supported range [1.16.1 - 1.18.2].";
            } else {
                oss << " is outside supported range [1.16.1 - 1.18.2].";
            }
            LogCategory("init", oss.str());
        } else {
            LogCategory("init", "No game version detected from command line.");
        }

        LoadConfig();

        LoadLangs();
        LogCategory("init", "Languages list loaded.");

        if (!LoadTranslation(g_config.lang)) {
            Log("FATAL: Could not load translations of " + g_config.lang);
            return TRUE;
        }
        LogCategory("init", "Loaded translations for language: " + g_config.lang);

        WCHAR dir[MAX_PATH];
        if (GetCurrentDirectoryW(MAX_PATH, dir) > 0) {
            g_stateFilePath = std::wstring(dir) + L"\\wpstateout.txt";
            LogCategory("init", "State file path set to: " + WideToUtf8(g_stateFilePath));

            DWORD stateFileAttrs = GetFileAttributesW(g_stateFilePath.c_str());
            bool stateOutputAvailable = (stateFileAttrs != INVALID_FILE_ATTRIBUTES) && !(stateFileAttrs & FILE_ATTRIBUTE_DIRECTORY);
            g_isStateOutputAvailable.store(stateOutputAvailable, std::memory_order_release);
            if (!stateOutputAvailable) {
                LogCategory(
                    "init",
                    "WARNING: wpstateout.txt not found. Game-state hotkey restrictions will not apply until State Output is installed.");
            }
        } else {
            Log("FATAL: Could not get current directory for state file path.");
        }

        // Use std::thread instead of CreateThread to ensure proper CRT per-thread
        // initialization (locale facets, errno, etc.). CreateThread skips CRT init which
        g_monitorThread = std::thread([]() { FileMonitorThread(nullptr); });
        g_imageMonitorThread = std::thread([]() { ImageMonitorThread(nullptr); });
        if (g_config.ninjabrainOverlay.enabled) {
            StartNinjabrainClient();
        }

        StartWindowCaptureThread();
        StartBrowserOverlayThread();

        if (MH_Initialize() != MH_OK) {
            Log("ERROR: MH_Initialize() failed!");
            return TRUE;
        }

        LogCategory("init", "Setting up hooks...");

        HMODULE hOpenGL32 = GetModuleHandle(L"opengl32.dll");
        HMODULE hUser32 = GetModuleHandle(L"user32.dll");
        HMODULE hGlfw = GetModuleHandle(L"glfw.dll");

        if (!hOpenGL32) {
            Log("ERROR: GetModuleHandle(opengl32.dll) returned NULL");
            return TRUE;
        }
        if (!hUser32) {
            Log("ERROR: GetModuleHandle(user32.dll) returned NULL");
            return TRUE;
        }

#define HOOK(mod, name) CreateHookOrDie(GetProcAddress(mod, #name), &hk##name, &o##name, #name)
        HOOK(hOpenGL32, wglSwapBuffers);
        HOOK(hOpenGL32, glBindTexture);
        if (IsVersionInRange(g_gameVersion, GameVersion(1, 0, 0), GameVersion(1, 21, 0))) {
            if (HOOK(hOpenGL32, glViewport)) {
                g_glViewportHookCount.fetch_add(1);
                LogCategory("init", "Initial glViewport hook created via opengl32.dll");
            }
        }
        HOOK(hUser32, SetCursorPos);
        HOOK(hUser32, GetClientRect);
        HOOK(hUser32, ClipCursor);
        HOOK(hUser32, SetCursor);
        HOOK(hUser32, GetRawInputData);
        if (hGlfw) {
            HOOK(hGlfw, glfwSetInputMode);
        } else {
            LogCategory("init", "WARNING: glfw.dll not loaded; skipping glfwSetInputMode hook");
        }
#undef HOOK

        LPVOID pGlBindFramebuffer = GetProcAddress(hOpenGL32, "glBindFramebuffer");
        if (pGlBindFramebuffer != NULL) {
            CreateHookOrDie(pGlBindFramebuffer, &hkglBindFramebuffer, &oglBindFramebuffer, "glBindFramebuffer");
        } else {
            LogCategory("init",
                        "WARNING: glBindFramebuffer not found in opengl32.dll - will attempt to hook via WGL/GLEW after context init");
        }

        LPVOID pGlBlitNamedFramebuffer = GetProcAddress(hOpenGL32, "glBlitNamedFramebuffer");
        if (pGlBlitNamedFramebuffer != NULL) {
            CreateHookOrDie(pGlBlitNamedFramebuffer, &hkglBlitNamedFramebuffer, &oglBlitNamedFramebuffer, "glBlitNamedFramebuffer");
        } else {
            LogCategory("init",
                        "WARNING: glBlitNamedFramebuffer not found in opengl32.dll - will attempt to hook via GLEW after context init");
        }

        LPVOID pGlBlitFramebuffer = GetProcAddress(hOpenGL32, "glBlitFramebuffer");
        if (pGlBlitFramebuffer != NULL) {
            if (CreateHookOrDie(pGlBlitFramebuffer, &hkglBlitFramebuffer, &oglBlitFramebuffer, "glBlitFramebuffer")) {
                g_glBlitFramebufferHooked.store(true, std::memory_order_release);
            }
        } else {
            LogCategory("init",
                        "WARNING: glBlitFramebuffer not found in opengl32.dll - will attempt to hook via WGL/GLEW after context init");
        }

        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
            Log("ERROR: MH_EnableHook(MH_ALL_HOOKS) failed!");
            return TRUE;
        }

        LogCategory("init", "Hooks enabled.");

        // This thread periodically detects those detours (prolog or IAT) and chains behind them.
        g_stopHookCompat.store(false, std::memory_order_release);
        g_hookCompatThread = std::thread([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            while (!g_stopHookCompat.load(std::memory_order_acquire) && !g_isShuttingDown.load(std::memory_order_acquire)) {
                HookChain::RefreshAllThirdPartyHookChains();

                for (int i = 0; i < 20; i++) {
                    if (g_stopHookCompat.load(std::memory_order_acquire) || g_isShuttingDown.load(std::memory_order_acquire)) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });

        // Save the original Windows mouse speed so we can restore it on exit
        SaveOriginalWindowsMouseSpeed();

        SaveOriginalKeyRepeatSettings();

    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        // We should do MINIMAL cleanup here. Windows will automatically clean up:
        // - GPU resources (driver handles cleanup)
        // - Thread handles
        // 1. Other threads may still be running

        g_isShuttingDown = true;
        Log("DLL Detached. Performing minimal cleanup...");

        if (g_highResTimer) {
            CloseHandle(g_highResTimer);
            g_highResTimer = NULL;
        }

        // ONLY save config and stop our own threads
        // Do NOT touch hooks, GPU resources, or game state

        // Restore original Windows mouse speed before exiting
        RestoreWindowsMouseSpeed();

        RestoreKeyRepeatSettings();

        SaveConfigImmediate();
        Log("Config saved.");

        // Stop monitoring threads
        g_stopMonitoring = true;
        if (g_monitorThread.joinable()) { g_monitorThread.join(); }

        g_stopImageMonitoring = true;
        if (g_imageMonitorThread.joinable()) { g_imageMonitorThread.join(); }

        // Stop hook compatibility monitor thread
        g_stopHookCompat.store(true, std::memory_order_release);
        if (g_hookCompatThread.joinable()) { g_hookCompatThread.join(); }
        StopNinjabrainClient();

        // Stop background threads
        StopBrowserOverlayThread();
        StopWindowCaptureThread();

        Log("Background threads stopped.");

        // Clean up CPU-allocated memory that won't be freed by Windows
        {
            std::lock_guard<std::mutex> lock(g_decodedImagesMutex);
            for (auto& decodedImg : g_decodedImagesQueue) {
                if (decodedImg.data) { stbi_image_free(decodedImg.data); }
            }
            g_decodedImagesQueue.clear();
        }

        // DO NOT:
        // - Delete GPU resources (Windows/driver handles this)

        Log("DLL cleanup complete (minimal cleanup strategy).");

        // Stop async logging thread and flush all pending logs
        StopLogThread();
        FlushLogs();

        {
            std::lock_guard<std::mutex> lock(g_logFileMutex);
            if (logFile.is_open()) {
                logFile.flush();
                logFile.close();
            }
        }

        ReleaseLatestLogSession(g_logSession);
    }
    return TRUE;
}
