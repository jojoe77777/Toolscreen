#include "fake_cursor.h"
#include "gui.h"
#include "imgui_cache.h"
#include "input_hook.h"
#include "logic_thread.h"
#include "mirror_thread.h"
#include "obs_thread.h"
#include "profiler.h"
#include "render.h"
#include "render_thread.h"
#include "resource.h"
#include "shared_contexts.h"
#include "utils.h"
#include "version.h"
#include "virtual_camera.h"
#include "window_overlay.h"

#include "MinHook.h"
#include <DbgHelp.h>
#include <Psapi.h>
#include <ShlObj.h>
#include <cmath>
#include <cstring>
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
#include <windowsx.h>

#define STB_IMAGE_IMPLEMENTATION
#include <imgui_impl_opengl3.h>
#include <imgui_impl_win32.h>
#include "stb_image.h"

Config g_config;
std::atomic<bool> g_configIsDirty{ false };

// Monotonic version for published config snapshots.
// Readers can use this to avoid recomputing derived state when config hasn't changed.
std::atomic<uint64_t> g_configSnapshotVersion{ 0 };

// ============================================================================
// CONFIG SNAPSHOT (RCU) - Lock-free immutable config for reader threads
// ============================================================================
// The mutable g_config is only touched by the GUI/main thread.
// After any mutation, PublishConfigSnapshot() copies it into a shared_ptr.
// Reader threads call GetConfigSnapshot() for a safe, lock-free snapshot.
// ============================================================================
static std::shared_ptr<const Config> g_configSnapshot;

void PublishConfigSnapshot() {
    auto snapshot = std::make_shared<const Config>(g_config);
    // Lock-free publish: atomic store of shared_ptr.
    std::atomic_store_explicit(&g_configSnapshot, std::move(snapshot), std::memory_order_release);

    // Bump version AFTER publishing.
    g_configSnapshotVersion.fetch_add(1, std::memory_order_release);
}

std::shared_ptr<const Config> GetConfigSnapshot() {
    // Lock-free read: atomic load of shared_ptr.
    return std::atomic_load_explicit(&g_configSnapshot, std::memory_order_acquire);
}

// ============================================================================
// HOTKEY SECONDARY MODE STATE - Thread-safe runtime state separated from Config
// ============================================================================
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

// ============================================================================
// TEMPORARY SENSITIVITY OVERRIDE - Set by sensitivity hotkeys, cleared on mode change
// ============================================================================
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
std::atomic<bool> g_configLoaded{ false }; // Set to true once LoadConfig() completes successfully
std::map<std::string, std::chrono::steady_clock::time_point> g_hotkeyTimestamps;
std::atomic<bool> g_guiNeedsRecenter{ true };
std::atomic<bool> g_wasCursorVisible{ true };
// Lock-free GUI toggle debounce timestamp
std::atomic<int64_t> g_lastGuiToggleTimeMs{ 0 };

enum CapturingState { NONE = 0, DISABLED = 1, NORMAL = 2 };
std::atomic<CapturingState> g_capturingMousePos{ CapturingState::NONE };
std::atomic<std::pair<int, int>> g_nextMouseXY{ std::make_pair(-1, -1) };

std::set<DWORD> g_hotkeyMainKeys;
std::mutex g_hotkeyMainKeysMutex;

std::mutex g_hotkeyTimestampsMutex;

// Track trigger-on-release hotkeys that are currently pressed
// Key is the hotkey ID string (from GetKeyComboString)
std::set<std::string> g_triggerOnReleasePending;
// Track which pending trigger-on-release hotkeys have been invalidated
// (another key was pressed while the hotkey was held)
std::set<std::string> g_triggerOnReleaseInvalidated;
std::mutex g_triggerOnReleaseMutex;

std::atomic<bool> g_imageDragMode{ false };
std::string g_draggedImageName = "";
std::mutex g_imageDragMutex;

std::atomic<bool> g_windowOverlayDragMode{ false };

std::ofstream logFile;
std::mutex g_logFileMutex;
HMODULE g_hModule = NULL;

GameVersion g_gameVersion;

bool g_glewLoaded = false;
WNDPROC g_originalWndProc = NULL;
std::atomic<HWND> g_subclassedHwnd{ NULL }; // Track which window is currently subclassed
std::atomic<bool> g_hwndChanged{ false };   // Signal that HWND changed (for ImGui reinit etc.)
std::atomic<bool> g_isShuttingDown{ false };
std::atomic<bool> g_allImagesLoaded{ false };
std::atomic<bool> g_isTransitioningMode{ false };
std::atomic<bool> g_skipViewportAnimation{ false }; // When true, viewport hook uses target position (for animations)
std::atomic<int> g_wmMouseMoveCount{ 0 };

static std::atomic<HGLRC> g_lastSeenGameGLContext{ NULL };

ModeTransitionAnimation g_modeTransition;
std::mutex g_modeTransitionMutex;
// Lock-free snapshot for viewport hook
ViewportTransitionSnapshot g_viewportTransitionSnapshots[2];
std::atomic<int> g_viewportTransitionSnapshotIndex{ 0 };

PendingModeSwitch g_pendingModeSwitch;
std::mutex g_pendingModeSwitchMutex;

PendingDimensionChange g_pendingDimensionChange;
std::mutex g_pendingDimensionChangeMutex;

std::atomic<double> g_lastFrameTimeMs{ 0.0 };
std::atomic<double> g_originalFrameTimeMs{ 0.0 };

std::chrono::high_resolution_clock::time_point g_lastFrameEndTime = std::chrono::high_resolution_clock::now();
std::mutex g_fpsLimitMutex;
HANDLE g_highResTimer = NULL;                             // High-resolution waitable timer for FPS limiting
int g_originalWindowsMouseSpeed = 0;                      // Original Windows mouse speed to restore on exit
std::atomic<bool> g_windowsMouseSpeedApplied{ false };    // Track if we've applied our speed setting
FILTERKEYS g_originalFilterKeys = { sizeof(FILTERKEYS) }; // Original FILTERKEYS state to restore on exit
std::atomic<bool> g_filterKeysApplied{ false };
std::atomic<bool> g_originalFilterKeysCaptured{ false }; // Track if original FILTERKEYS snapshot has been captured

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
HANDLE g_resizeThread = NULL;
std::atomic<bool> g_stopMonitoring{ false };
std::atomic<bool> g_stopImageMonitoring{ false };
std::wstring g_stateFilePath;
std::atomic<bool> g_isStateOutputAvailable{ false };

std::vector<DecodedImageData> g_decodedImagesQueue;
std::mutex g_decodedImagesMutex;

// Use UINT_MAX as sentinel value for "not yet initialized"
// This allows 0 to be a valid texture ID
std::atomic<GLuint> g_cachedGameTextureId{ UINT_MAX };

std::atomic<HCURSOR> g_specialCursorHandle{ NULL };

std::atomic<bool> g_graphicsHookDetected{ false };
std::atomic<HMODULE> g_graphicsHookModule{ NULL };
std::chrono::steady_clock::time_point g_lastGraphicsHookCheck = std::chrono::steady_clock::now();
extern const int GRAPHICS_HOOK_CHECK_INTERVAL_MS = 2000;

std::atomic<bool> g_obsCaptureReady{ false };

void LoadConfig();
void SaveConfig();
void RenderSettingsGUI();
void AttemptAggressiveGlViewportHook();
GLuint CalculateGameTextureId(int windowWidth, int windowHeight, int fullWidth, int fullHeight);

bool SubclassGameWindow(HWND hwnd) {
    if (!hwnd) return false;

    // Don't subclass if already shutting down
    if (g_isShuttingDown.load()) return false;

    // Check if we already subclassed this window
    HWND currentSubclassed = g_subclassedHwnd.load();
    if (currentSubclassed == hwnd && g_originalWndProc != NULL) {
        // Already subclassed this window
        return true;
    }

    // If we have a different window subclassed, log the transition and signal state reset
    if (currentSubclassed != NULL && currentSubclassed != hwnd) {
        Log("Window handle changed from " + std::to_string(reinterpret_cast<uintptr_t>(currentSubclassed)) + " to " +
            std::to_string(reinterpret_cast<uintptr_t>(hwnd)) + " (likely fullscreen toggle)");
        // Note: We don't restore the old window proc because the old window is likely destroyed
        g_originalWndProc = NULL; // Reset to allow new subclassing

        // Update global HWND and signal for state reset (ImGui reinit, texture cache invalidation, etc.)
        g_minecraftHwnd.store(hwnd);
        g_cachedGameTextureId.store(UINT_MAX); // Force texture recalculation
        g_hwndChanged.store(true);             // Signal ImGui backends to reinitialize
    }

    // Subclass the new window
    WNDPROC oldProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassedWndProc);
    if (oldProc) {
        g_originalWndProc = oldProc;
        g_subclassedHwnd.store(hwnd);
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

// Internal function to rebuild hotkey main keys cache
// REQUIRES: g_configMutex and g_hotkeyMainKeysMutex must already be held by caller
void RebuildHotkeyMainKeys_Internal() {
    g_hotkeyMainKeys.clear();

    // Helper lambda to check if a key is a modifier
    auto isModifier = [](DWORD key) {
        return key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL || key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
               key == VK_MENU || key == VK_LMENU || key == VK_RMENU;
    };

    // Helper lambda to add the main key from a key list
    // The main key is the last key in the list, OR if all keys are modifiers,
    // the last modifier is treated as the main key
    auto addMainKey = [&](const std::vector<DWORD>& keys) {
        if (keys.empty()) return;
        DWORD mainKey = keys.back();
        g_hotkeyMainKeys.insert(mainKey);

        // For modifier keys, also add the generic version since Windows sends
        // VK_CONTROL/VK_SHIFT/VK_MENU in wParam, not the left/right-specific codes
        if (mainKey == VK_LCONTROL || mainKey == VK_RCONTROL) {
            g_hotkeyMainKeys.insert(VK_CONTROL);
        } else if (mainKey == VK_CONTROL) {
            // If the binding uses the generic modifier, also accept left/right variants
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

    // Extract main keys from all hotkey configurations
    for (const auto& hotkey : g_config.hotkeys) {
        // Main hotkey
        addMainKey(hotkey.keys);

        // Alt secondary mode hotkeys
        for (const auto& alt : hotkey.altSecondaryModes) { addMainKey(alt.keys); }
    }

    // Extract main keys from sensitivity hotkeys
    for (const auto& sensHotkey : g_config.sensitivityHotkeys) { addMainKey(sensHotkey.keys); }

    // Also include GUI hotkey
    addMainKey(g_config.guiHotkey);

    // Also include borderless toggle hotkey (optional)
    addMainKey(g_config.borderlessHotkey);

    // Always include Escape as it can toggle GUI
    g_hotkeyMainKeys.insert(VK_ESCAPE);

    // Include key rebinds so they're not skipped by the early exit optimization
    if (g_config.keyRebinds.enabled) {
        for (const auto& rebind : g_config.keyRebinds.rebinds) {
            if (rebind.enabled && rebind.fromKey != 0) {
                g_hotkeyMainKeys.insert(rebind.fromKey);

                // Mirror the modifier normalization rules so rebinding VK_RSHIFT still works even though
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

// Public function to rebuild the set of main keys used in hotkey bindings
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
        g_originalWindowsMouseSpeed = 10; // Default to middle value
    }
}

// Apply the configured Windows mouse speed (if enabled)
void ApplyWindowsMouseSpeed() {
    int targetSpeed = g_config.windowsMouseSpeed;

    if (targetSpeed == 0) {
        // Feature disabled - restore original speed if we had applied ours
        if (g_windowsMouseSpeedApplied.load()) {
            if (SystemParametersInfo(SPI_SETMOUSESPEED, 0, reinterpret_cast<void*>(static_cast<intptr_t>(g_originalWindowsMouseSpeed)),
                                     0)) {
                Log("Restored Windows mouse speed to: " + std::to_string(g_originalWindowsMouseSpeed));
            }
            g_windowsMouseSpeedApplied.store(false);
        }
        return;
    }

    // Clamp to valid range
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

// Save the original key repeat settings (FILTERKEYS)
void SaveOriginalKeyRepeatSettings() {
    g_originalFilterKeys.cbSize = sizeof(FILTERKEYS);
    if (SystemParametersInfo(SPI_GETFILTERKEYS, sizeof(FILTERKEYS), &g_originalFilterKeys, 0)) {
        g_originalFilterKeysCaptured.store(true);
        LogCategory("init", "Saved original FILTERKEYS: flags=0x" + std::to_string(g_originalFilterKeys.dwFlags) +
                                ", iDelayMSec=" + std::to_string(g_originalFilterKeys.iDelayMSec) +
                                ", iRepeatMSec=" + std::to_string(g_originalFilterKeys.iRepeatMSec));
    } else {
        Log("WARNING: Failed to get current FILTERKEYS settings");
        // Initialize with defaults
        g_originalFilterKeys.dwFlags = 0;
        g_originalFilterKeys.iDelayMSec = 0;
        g_originalFilterKeys.iRepeatMSec = 0;
        g_originalFilterKeysCaptured.store(false);
    }
}

// Apply the configured key repeat settings (if enabled)
void ApplyKeyRepeatSettings() {
    // Ensure we have a baseline snapshot to restore from even if apply is called early.
    if (!g_originalFilterKeysCaptured.load(std::memory_order_acquire)) { SaveOriginalKeyRepeatSettings(); }

    int startDelay = g_config.keyRepeatStartDelay;
    int repeatDelay = g_config.keyRepeatDelay;

    // Check if either setting is enabled (non-zero)
    if (startDelay == 0 && repeatDelay == 0) {
        // Both disabled - restore original settings if we had applied ours
        if (g_filterKeysApplied.load()) {
            if (SystemParametersInfo(SPI_SETFILTERKEYS, sizeof(FILTERKEYS), &g_originalFilterKeys, 0)) {
                Log("Restored original FILTERKEYS settings");
            }
            g_filterKeysApplied.store(false);
        }
        return;
    }

    // Clamp to valid range (1-500ms)
    if (startDelay < 0) startDelay = 0;
    if (startDelay > 500) startDelay = 500;
    if (repeatDelay < 0) repeatDelay = 0;
    if (repeatDelay > 500) repeatDelay = 500;

    // Build FILTERKEYS structure with our custom settings
    FILTERKEYS fk = { sizeof(FILTERKEYS) };
    fk.dwFlags = FKF_FILTERKEYSON;                                                       // Enable filter keys
    fk.iWaitMSec = 0;                                                                    // No wait before accepting keystrokes
    fk.iDelayMSec = (startDelay > 0) ? startDelay : g_originalFilterKeys.iDelayMSec;     // Delay before repeat starts
    fk.iRepeatMSec = (repeatDelay > 0) ? repeatDelay : g_originalFilterKeys.iRepeatMSec; // Time between repeats
    fk.iBounceMSec = 0;                                                                  // No bounce time

    if (SystemParametersInfo(SPI_SETFILTERKEYS, sizeof(FILTERKEYS), &fk, 0)) {
        g_filterKeysApplied.store(true);
        Log("Applied key repeat settings: startDelay=" + std::to_string(fk.iDelayMSec) +
            "ms, repeatDelay=" + std::to_string(fk.iRepeatMSec) + "ms");
    } else {
        Log("WARNING: Failed to set key repeat settings");
    }
}

// Restore the original key repeat settings on shutdown or focus loss
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
typedef BOOL(WINAPI* PFNWGLDELETECONTEXTPROC)(HGLRC);
PFNWGLDELETECONTEXTPROC owglDeleteContext = NULL;
typedef BOOL(WINAPI* SETCURSORPOSPROC)(int, int);
SETCURSORPOSPROC oSetCursorPos = NULL;
typedef BOOL(WINAPI* CLIPCURSORPROC)(const RECT*);
CLIPCURSORPROC oClipCursor = NULL;
typedef HCURSOR(WINAPI* SETCURSORPROC)(HCURSOR);
SETCURSORPROC oSetCursor = NULL;
typedef void(WINAPI* GLVIEWPORTPROC)(GLint x, GLint y, GLsizei width, GLsizei height);
GLVIEWPORTPROC oglViewport = NULL;
typedef void(WINAPI* GLCLEARPROC)(GLbitfield mask);
GLCLEARPROC oglClear = NULL;

// Thread-local flag to track if glViewport is being called from our own code
thread_local bool g_internalViewportCall = false;

// Multiple glViewport hook targets for aggressive hooking (AMD GPU compatibility)
std::atomic<int> g_glViewportHookCount{ 0 };
std::atomic<bool> g_glViewportHookedViaGLEW{ false };
std::atomic<bool> g_glViewportHookedViaWGL{ false };

typedef void(WINAPI* GLBLITNAMEDFRAMEBUFFERPROC)(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1,
                                                 GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask,
                                                 GLenum filter);
GLBLITNAMEDFRAMEBUFFERPROC oglBlitNamedFramebuffer = NULL;

// glBlitFramebuffer hook for OBS graphics-hook detection
typedef void(APIENTRY* PFNGLBLITFRAMEBUFFERPROC_HOOK)(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0,
                                                      GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);
PFNGLBLITFRAMEBUFFERPROC_HOOK oglBlitFramebuffer = NULL;
std::atomic<bool> g_glBlitFramebufferHooked{ false };

typedef void (*GLFWSETINPUTMODE)(void* window, int mode, int value);
GLFWSETINPUTMODE oglfwSetInputMode = NULL;

// GetRawInputData hook for mouse sensitivity
typedef UINT(WINAPI* GETRAWINPUTDATAPROC)(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
GETRAWINPUTDATAPROC oGetRawInputData = NULL;

BOOL WINAPI hkClipCursor(const RECT* lpRect) {
    // When GUI is open, always allow cursor to move freely (even to other monitors)
    if (g_showGui.load()) { return oClipCursor(NULL); }

    // For 1.13.0+, just pass through the original rect
    if (g_gameVersion >= GameVersion(1, 13, 0)) { return oClipCursor(lpRect); }

    // For < 1.13.0, check toggle to decide whether to allow cursor escape
    // Note: Reading scalar bool directly - pragmatically safe on x86-64 for aligned bools.
    // hkClipCursor is called too frequently to justify a full snapshot per call.
    if (g_config.allowCursorEscape) {
        return oClipCursor(NULL); // Allow cursor to escape (pass NULL)
    }
    return oClipCursor(lpRect); // Confine cursor (pass original rect)
}

HCURSOR WINAPI hkSetCursor(HCURSOR hCursor) {
    if (g_gameVersion >= GameVersion(1, 13, 0)) { return oSetCursor(hCursor); }

    std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];

    if (g_showGui.load()) {
        const CursorTextures::CursorData* cursorData = CursorTextures::GetSelectedCursor(localGameState, 64);
        if (cursorData && cursorData->hCursor) { return oSetCursor(cursorData->hCursor); }
    }

    // If we've already found the special cursor, skip checking
    if (g_specialCursorHandle.load() != NULL) { return oSetCursor(hCursor); }

    // Check if mask hash of new cursor is "773ff800"
    ICONINFO ii = { sizeof(ICONINFO) };
    if (GetIconInfo(hCursor, &ii)) {
        BITMAP bitmask = {};
        GetObject(ii.hbmMask, sizeof(BITMAP), &bitmask);

        // Compute hash of hbmMask using same algorithm as utils.cpp
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

        // If mask hash is "773ff800", cache it
        if (maskHash == "773ff800") {
            Log("hkSetCursor: Detected special cursor (maskHash=773ff800), caching for later use");
            g_specialCursorHandle.store(hCursor);
        }

        // Clean up ICONINFO bitmaps
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
    }

    return oSetCursor(hCursor);
}
// Note: OBS capture is now handled by obs_thread.cpp via glBlitFramebuffer hook
// Old functions EnsureObsCaptureFBO, CaptureToObsFBO, and OBS blit redirection removed

static int lastViewportW = 0;
static int lastViewportH = 0;

void WINAPI hkglViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
    if (!IsFullscreen()) {
        // Log("viewport not fullscreen");
        return oglViewport(x, y, width, height);
    }

    // Lock-free read of transition snapshot
    const ViewportTransitionSnapshot& transitionSnap =
        g_viewportTransitionSnapshots[g_viewportTransitionSnapshotIndex.load(std::memory_order_acquire)];
    bool isTransitionActive = transitionSnap.active;

    // Lock-free read of cached mode viewport data (updated by logic_thread)
    const CachedModeViewport& cachedMode = g_viewportModeCache[g_viewportModeCacheIndex.load(std::memory_order_acquire)];

    // During transitions, we can derive dimensions from the transition snapshot even if cache is stale.
    // The snapshot is updated synchronously on mode switch, while cache has ~16ms lag.
    int modeWidth, modeHeight;
    bool stretchEnabled;
    int stretchX, stretchY, stretchWidth, stretchHeight;

    if (isTransitionActive) {
        // Use transition snapshot's NATIVE dimensions for matching - game's glViewport uses native size
        // The stretch dimensions are only used for actual viewport positioning
        modeWidth = transitionSnap.toNativeWidth;
        modeHeight = transitionSnap.toNativeHeight;
        // For stretch, use target position/size from snapshot
        stretchEnabled = true; // Transition implies stretching to target position
        stretchX = transitionSnap.toX;
        stretchY = transitionSnap.toY;
        stretchWidth = transitionSnap.toWidth;
        stretchHeight = transitionSnap.toHeight;
    } else if (cachedMode.valid) {
        // Not transitioning - use cached mode data
        modeWidth = cachedMode.width;
        modeHeight = cachedMode.height;
        stretchEnabled = cachedMode.stretchEnabled;
        stretchX = cachedMode.stretchX;
        stretchY = cachedMode.stretchY;
        stretchWidth = cachedMode.stretchWidth;
        stretchHeight = cachedMode.stretchHeight;
    } else {
        // Cache not yet populated and no transition - fall back to original viewport call
        return oglViewport(x, y, width, height);
    }

    bool posValid = x == 0 && y == 0;
    bool widthMatches = (width == modeWidth) || (width == lastViewportW);
    bool heightMatches = (height == modeHeight) || (height == lastViewportH);

    // During transition, also accept FROM and TO NATIVE dimensions (from snapshot)
    // FROM: the first viewport call may still be at old dimensions
    // TO: WM_SIZE is sent immediately, so game may already be at target dimensions
    // Use native dimensions since that's what glViewport receives from the game
    if (isTransitionActive && (!widthMatches || !heightMatches)) {
        widthMatches = widthMatches || (width == transitionSnap.fromNativeWidth) || (width == transitionSnap.toNativeWidth);
        heightMatches = heightMatches || (height == transitionSnap.fromNativeHeight) || (height == transitionSnap.toNativeHeight);
    }

    if (!posValid || !widthMatches || !heightMatches) {
        /*Log("Returning because viewport parameters don't match mode (x=" + std::to_string(x) + ", y=" + std::to_string(y) +
            ", width=" + std::to_string(width) + ", height=" + std::to_string(height) +
            "), lastViewportW=" + std::to_string(lastViewportW) + ", lastViewportH=" + std::to_string(lastViewportH) +
            ", modeWidth=" + std::to_string(modeWidth) + ", modeHeight=" + std::to_string(modeHeight) + ")");*/
        return oglViewport(x, y, width, height);
    }

    GLint readFBO = 0;
    GLint currentTexture = 0;

    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFBO);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &currentTexture);

    if (currentTexture == 0 || readFBO != 0) {
        // Log("Returning because no texture is bound or FBO is bound (fb binding =" + std::to_string(readFBO) +
        //     " and tex binding=" + std::to_string(currentTexture) + ")");
        return oglViewport(x, y, width, height);
    }

    lastViewportW = modeWidth;
    lastViewportH = modeHeight;

    const int screenW = GetCachedScreenWidth();
    const int screenH = GetCachedScreenHeight();

    // Check if mode transition animation is active (from snapshot - no lock needed)
    // For Move transitions: use TARGET position so game renders at final location,
    // then RenderModeInternal will blit it to the animated position.
    // This prevents stretching the entire framebuffer including GUI/overlays.
    bool useAnimatedDimensions = transitionSnap.active;
    bool isMoveTransition = transitionSnap.isBounceTransition;
    int animatedX = transitionSnap.currentX;
    int animatedY = transitionSnap.currentY;
    int animatedWidth = transitionSnap.currentWidth;
    int animatedHeight = transitionSnap.currentHeight;
    int targetX = transitionSnap.toX;
    int targetY = transitionSnap.toY;
    int targetWidth = transitionSnap.toWidth;
    int targetHeight = transitionSnap.toHeight;

    if (useAnimatedDimensions) {
        // Check if we should skip animation (for "Hide Animations in Game" feature)
        // Note: Reading scalar bool directly from g_config is pragmatically safe on x86-64
        // (no tearing for aligned bools). A full config snapshot per glViewport call would be too expensive.
        bool shouldSkipAnimation = g_config.hideAnimationsInGame;

        if (shouldSkipAnimation) {
            // With hideAnimationsInGame: render game at TARGET position immediately
            // OBS capture handles its own animated blitting separately.
            stretchX = targetX;
            stretchY = targetY;
            stretchWidth = targetWidth;
            stretchHeight = targetHeight;
        } else {
            // Use animated position - game renders directly at the animated location on screen
            stretchX = animatedX;
            stretchY = animatedY;
            stretchWidth = animatedWidth;
            stretchHeight = animatedHeight;
        }
    } else {
        // Not animating - use mode's stretch settings directly if enabled
        if (!stretchEnabled) {
            // No stretch configured - center the game viewport
            stretchX = screenW / 2 - modeWidth / 2;
            stretchY = screenH / 2 - modeHeight / 2;
            stretchWidth = modeWidth;
            stretchHeight = modeHeight;
        }
        // else: stretchX/Y/Width/Height already set from mode config above
        // Log("[VIEWPORT] Using static stretch: " + std::to_string(stretchWidth) + "x" + std::to_string(stretchHeight) + " at " +
        //    std::to_string(stretchX) + "," + std::to_string(stretchY) + " (enabled: " + std::to_string(stretchEnabled) + ")");
    }

    // Convert Y coordinate from Windows screen space (top-left origin) to OpenGL viewport space (bottom-left origin)
    int stretchY_gl = screenH - stretchY - stretchHeight;

    // Log("Viewport Hook: setting viewport to " + std::to_string(stretchWidth) + "x" + std::to_string(stretchHeight) + " at (" +
    //     std::to_string(stretchX) + "," + std::to_string(stretchY_gl) + ")");

    return oglViewport(stretchX, stretchY_gl, stretchWidth, stretchHeight);
}

// Forward declaration for dynamic hooking in hkglClear
void WINAPI hkglBlitNamedFramebuffer(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                     GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter);

thread_local bool glewInitializedInHook = false;
thread_local bool glBlitNamedFramebufferHooked = false;

void WINAPI hkglClear(GLbitfield mask) {
    // return oglClear(mask);
    // if (g_cachedGameTextureId.load() != UINT_MAX) { return; } // Already have a cached texture ID, skip
    if (!glewInitializedInHook) {
        // This flag is essential for core profile contexts. It tells GLEW to
        // try more modern ways of getting function pointers.
        glewExperimental = GL_TRUE;
        if (glewInit() == GLEW_OK) {
            glewInitializedInHook = true;

            // Now that GLEW is initialized, hook glBlitNamedFramebuffer if not already hooked
            if (!glBlitNamedFramebufferHooked && oglBlitNamedFramebuffer == NULL) {
                PFNGLBLITNAMEDFRAMEBUFFERPROC pFunc = glBlitNamedFramebuffer;
                if (pFunc != NULL) {
                    if (MH_CreateHook(pFunc, &hkglBlitNamedFramebuffer, reinterpret_cast<void**>(&oglBlitNamedFramebuffer)) == MH_OK) {
                        if (MH_EnableHook(pFunc) == MH_OK) {
                            glBlitNamedFramebufferHooked = true;
                            LogCategory("init", "Successfully hooked glBlitNamedFramebuffer via GLEW");
                        } else {
                            Log("ERROR: Failed to enable glBlitNamedFramebuffer hook");
                        }
                    } else {
                        Log("ERROR: Failed to create glBlitNamedFramebuffer hook");
                    }
                } else {
                    Log("WARNING: glBlitNamedFramebuffer not available via GLEW");
                }
            }
        } else {
            Log("SCARY: glewInit() failed inside hkGlClear for the current context!");
            return;
        }
    }
    return oglClear(mask);

    // if (mask != GL_DEPTH_BUFFER_BIT) { return; }

    GLint attachmentType = GL_NONE;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                                          &attachmentType);

    if (attachmentType != GL_TEXTURE) {
        Log("Skipping GLclear hook, attachment type is not GL_TEXTURE: " + std::to_string(attachmentType) + ")");
        return;
    }

    GLint texId = 0;
    glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &texId);
    // Note: texId can legitimately be 0, so we don't skip on 0
    g_cachedGameTextureId.store(texId);
    Log("Calibrated game texture ID: " + std::to_string(texId));
}

BOOL WINAPI hkSetCursorPos(int X, int Y) {
    bool isFull = IsFullscreen();
    if (g_showGui.load() || g_isShuttingDown.load()) { return oSetCursorPos(X, Y); }

    ModeViewportInfo viewport = GetCurrentModeViewport();
    if (!viewport.valid) { return oSetCursorPos(X, Y); }

    CapturingState currentState = g_capturingMousePos.load();

    // IMPORTANT: SetCursorPos expects VIRTUAL SCREEN coordinates.
    // Our mode viewport coordinates are computed relative to the game monitor's (0,0),
    // so we must add the monitor's rcMonitor.left/top for multi-monitor setups.
    int centerX = viewport.stretchX + viewport.stretchWidth / 2;
    int centerY = viewport.stretchY + viewport.stretchHeight / 2;
    int centerX_abs = centerX;
    int centerY_abs = centerY;
    if (isFull) {
        RECT monRect{};
        HWND hwnd = g_minecraftHwnd.load();
        if (GetMonitorRectForWindow(hwnd, monRect)) {
            centerX_abs = monRect.left + centerX;
            centerY_abs = monRect.top + centerY;
        }
    }

    if (currentState == CapturingState::DISABLED) {
        if (isFull) {
            g_nextMouseXY.store(std::make_pair(centerX_abs, centerY_abs));
        } else {
            g_nextMouseXY.store(std::make_pair(X, Y));
        }
        return oSetCursorPos(X, Y);
    }

    if (currentState == CapturingState::NORMAL) {
        auto [expectedX, expectedY] = g_nextMouseXY.load();
        if (expectedX == -1 && expectedY == -1) { return oSetCursorPos(X, Y); }
        return oSetCursorPos(expectedX, expectedY);
    }

    // probably never happens, maybe if we SetCursorPos from elsewhere
    return oSetCursorPos(X, Y);
}

#define GLFW_CURSOR 0x00033001
#define GLFW_CURSOR_NORMAL 0x00034001
#define GLFW_CURSOR_HIDDEN 0x00034002
#define GLFW_CURSOR_DISABLED 0x00034003

void hkglfwSetInputMode(void* window, int mode, int value) {
    if (mode != GLFW_CURSOR) { return oglfwSetInputMode(window, mode, value); }

    if (value == GLFW_CURSOR_DISABLED) {
        g_capturingMousePos.store(CapturingState::DISABLED);
        // When GUI is open, don't actually disable/lock the cursor - let it move freely
        if (g_showGui.load()) {
            return; // Skip the call to keep cursor unlocked
        }
        oglfwSetInputMode(window, mode, value);
    } else if (value == GLFW_CURSOR_NORMAL) {
        g_capturingMousePos.store(CapturingState::NORMAL);
        oglfwSetInputMode(window, mode, value);
    } else { // probably never happens
        oglfwSetInputMode(window, mode, value);
    }

    g_capturingMousePos.store(CapturingState::NONE);
}

// Hook for GetRawInputData to apply mouse sensitivity multiplier and keyboard rebinds
UINT WINAPI hkGetRawInputData(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
    // Call original first
    UINT result = oGetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);

    // Raw input is being used - reset the WM_MOUSEMOVE counter
    g_wmMouseMoveCount.store(0);

    // Only modify if we got valid data
    if (result == static_cast<UINT>(-1) || pData == nullptr || uiCommand != RID_INPUT) { return result; }

    // Skip if GUI is open or shutting down
    if (g_showGui.load() || g_isShuttingDown.load()) { return result; }

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(pData);

    // Handle mouse sensitivity
    if (raw->header.dwType == RIM_TYPEMOUSE) {
        // Get sensitivity setting using LOCK-FREE access to avoid input delay
        // This is critical for low-latency input processing
        float sensitivityX = 1.0f;
        float sensitivityY = 1.0f;
        bool sensitivityDetermined = false;

        // Priority 1: Check for temporary sensitivity override (from sensitivity hotkeys)
        // This takes precedence over all other sensitivity settings until mode change
        {
            std::lock_guard<std::mutex> lock(g_tempSensitivityMutex);
            if (g_tempSensitivityOverride.active) {
                sensitivityX = g_tempSensitivityOverride.sensitivityX;
                sensitivityY = g_tempSensitivityOverride.sensitivityY;
                sensitivityDetermined = true;
            }
        }

        // Priority 2: Mode-specific or global sensitivity (if no temp override)
        if (!sensitivityDetermined) {
            // Lock-free read: check transition snapshot first
            const ViewportTransitionSnapshot& transitionSnap =
                g_viewportTransitionSnapshots[g_viewportTransitionSnapshotIndex.load(std::memory_order_acquire)];

            // Get mode ID: use target mode during transitions, otherwise current mode
            std::string modeId;
            if (transitionSnap.active) {
                modeId = transitionSnap.toModeId; // Target mode during transition (lock-free from snapshot)
            } else {
                // Lock-free read of current mode ID from double-buffer
                modeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
            }

            // Check if the mode has a sensitivity override (use snapshot for thread safety)
            auto inputCfgSnap = GetConfigSnapshot();
            const ModeConfig* mode = inputCfgSnap ? GetModeFromSnapshot(*inputCfgSnap, modeId) : nullptr;
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

        // Only process if sensitivity is different from default
        if (sensitivityX != 1.0f || sensitivityY != 1.0f) {
            // Only apply to relative mouse movement (not absolute positioning)
            if (!(raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                // Use accumulators to preserve fractional movements that would otherwise be lost
                // This prevents small movements from being truncated to zero with sub-1.0 sensitivity
                static thread_local float xAccum = 0.0f;
                static thread_local float yAccum = 0.0f;

                // Add scaled movement to accumulator
                xAccum += raw->data.mouse.lLastX * sensitivityX;
                yAccum += raw->data.mouse.lLastY * sensitivityY;

                // Extract integer portion for output
                LONG outputX = static_cast<LONG>(xAccum);
                LONG outputY = static_cast<LONG>(yAccum);

                // Keep fractional remainder for next frame
                xAccum -= outputX;
                yAccum -= outputY;

                raw->data.mouse.lLastX = outputX;
                raw->data.mouse.lLastY = outputY;
            }
        }
    }

    return result;
}

void WINAPI hkglBlitNamedFramebuffer(GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1,
                                     GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter) {
    bool isFull = IsFullscreen();
    if (!isFull) {
        return oglBlitNamedFramebuffer(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask,
                                       filter);
    }

    // Minecraft 1.21+ uses glBlitNamedFramebuffer extensively for internal post-processing blits between FBOs.
    // Our coordinate remap is ONLY intended for the final blit into the default framebuffer.
    // If we remap internal blits (drawFramebuffer != 0), we can corrupt the pipeline and end up with a black final frame.
    if (drawFramebuffer != 0) {
        return oglBlitNamedFramebuffer(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask,
                                       filter);
    }

    // Get the current mode's viewport information to determine proper destination coordinates
    ModeViewportInfo viewport = GetCurrentModeViewport();

    if (viewport.valid) {
        // Convert OpenGL Y coordinates (bottom-left origin) to screen Y coordinates (top-left origin)
        int screenH = GetCachedScreenHeight();
        int destY0_screen = screenH - viewport.stretchY - viewport.stretchHeight;
        int destY1_screen = screenH - viewport.stretchY;

        // Use the stretch dimensions as destination coordinates
        return oglBlitNamedFramebuffer(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, viewport.stretchX, destY0_screen,
                                       viewport.stretchX + viewport.stretchWidth, destY1_screen, mask, filter);
    }

    // Fallback to original parameters if viewport invalid or stretch disabled
    return oglBlitNamedFramebuffer(readFramebuffer, drawFramebuffer, srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
}

// Aggressive glViewport hooking for AMD GPU compatibility
// This function attempts multiple hooking strategies to ensure we catch all glViewport calls
void AttemptAggressiveGlViewportHook() {
    int hooksCreated = 0;

    // Strategy 1: Hook via GLEW (extension/driver-specific function pointer)
    if (!g_glViewportHookedViaGLEW.load()) {
        // glViewport is a core function, so we can get it directly
        GLVIEWPORTPROC pGlViewportGLEW = glViewport;
        if (pGlViewportGLEW != NULL && pGlViewportGLEW != oglViewport) {
            Log("Attempting glViewport hook via GLEW pointer: " + std::to_string(reinterpret_cast<uintptr_t>(pGlViewportGLEW)));
            if (MH_CreateHook(pGlViewportGLEW, &hkglViewport, reinterpret_cast<void**>(&oglViewport)) == MH_OK) {
                if (MH_EnableHook(pGlViewportGLEW) == MH_OK) {
                    g_glViewportHookedViaGLEW.store(true);
                    hooksCreated++;
                    Log("SUCCESS: glViewport hooked via GLEW");
                } else {
                    Log("ERROR: Failed to enable glViewport hook via GLEW");
                }
            } else {
                Log("ERROR: Failed to create glViewport hook via GLEW");
            }
        }
    }

    // Strategy 2: Hook via wglGetProcAddress (driver-specific implementation)
    if (!g_glViewportHookedViaWGL.load()) {
        typedef PROC(WINAPI * PFN_wglGetProcAddress)(LPCSTR);
        HMODULE hOpenGL32 = GetModuleHandle(L"opengl32.dll");
        if (hOpenGL32) {
            PFN_wglGetProcAddress pwglGetProcAddress =
                reinterpret_cast<PFN_wglGetProcAddress>(GetProcAddress(hOpenGL32, "wglGetProcAddress"));
            if (pwglGetProcAddress) {
                PROC pGlViewportWGL = pwglGetProcAddress("glViewport");
                if (pGlViewportWGL != NULL && reinterpret_cast<void*>(pGlViewportWGL) != reinterpret_cast<void*>(oglViewport) &&
                    reinterpret_cast<void*>(pGlViewportWGL) != reinterpret_cast<void*>(glViewport)) {
                    Log("Attempting glViewport hook via wglGetProcAddress: " + std::to_string(reinterpret_cast<uintptr_t>(pGlViewportWGL)));
                    GLVIEWPORTPROC pViewportFunc = reinterpret_cast<GLVIEWPORTPROC>(pGlViewportWGL);
                    if (MH_CreateHook(pViewportFunc, &hkglViewport, reinterpret_cast<void**>(&oglViewport)) == MH_OK) {
                        if (MH_EnableHook(pViewportFunc) == MH_OK) {
                            g_glViewportHookedViaWGL.store(true);
                            hooksCreated++;
                            Log("SUCCESS: glViewport hooked via wglGetProcAddress");
                        } else {
                            Log("ERROR: Failed to enable glViewport hook via wglGetProcAddress");
                        }
                    } else {
                        Log("ERROR: Failed to create glViewport hook via wglGetProcAddress");
                    }
                }
            }
        }
    }

    // Strategy 3: Try to hook all potential glViewport implementations in memory
    // This searches for the actual function in the loaded OpenGL driver (e.g., amdxxx.dll, nvoglv64.dll)
    HMODULE hModules[1024];
    DWORD cbNeeded;
    if (EnumProcessModules(GetCurrentProcess(), hModules, sizeof(hModules), &cbNeeded)) {
        DWORD numModules = cbNeeded / sizeof(HMODULE);
        for (DWORD i = 0; i < numModules; i++) {
            WCHAR moduleName[MAX_PATH];
            if (GetModuleFileNameW(hModules[i], moduleName, MAX_PATH)) {
                std::wstring moduleNameStr(moduleName);
                // Check if this is an OpenGL driver DLL (AMD or NVIDIA)
                if (moduleNameStr.find(L"atig") != std::wstring::npos || moduleNameStr.find(L"atio") != std::wstring::npos ||
                    moduleNameStr.find(L"amd") != std::wstring::npos || moduleNameStr.find(L"nvoglv") != std::wstring::npos ||
                    moduleNameStr.find(L"ig") != std::wstring::npos) { // Intel

                    // Try to get glViewport from this module
                    GLVIEWPORTPROC pDriverViewport = reinterpret_cast<GLVIEWPORTPROC>(GetProcAddress(hModules[i], "glViewport"));
                    if (pDriverViewport != NULL && pDriverViewport != oglViewport) {
                        Log("Found glViewport in driver module: " + WideToUtf8(moduleNameStr) + " at " +
                            std::to_string(reinterpret_cast<uintptr_t>(pDriverViewport)));
                        if (MH_CreateHook(pDriverViewport, &hkglViewport, reinterpret_cast<void**>(&oglViewport)) == MH_OK) {
                            if (MH_EnableHook(pDriverViewport) == MH_OK) {
                                hooksCreated++;
                                Log("SUCCESS: glViewport hooked in driver module: " + WideToUtf8(moduleNameStr));
                            }
                        }
                    }
                }
            }
        }
    }

    g_glViewportHookCount.fetch_add(hooksCreated);
    Log("Aggressive glViewport hooking complete. Total additional hooks created: " + std::to_string(hooksCreated));
    Log("Total glViewport hook count: " + std::to_string(g_glViewportHookCount.load()));
}

// Helper function to find the game texture ID by matching dimensions with current mode viewport
GLuint CalculateGameTextureId(int windowWidth, int windowHeight, int fullWidth, int fullHeight) {
    ModeViewportInfo viewport = GetCurrentModeViewport();
    if (!viewport.valid) {
        Log("CalculateGameTextureId: Invalid viewport, cannot calculate texture ID");
        return UINT_MAX;
    }

    int targetWidth = viewport.width;
    int targetHeight = viewport.height;

    if (windowWidth != fullWidth || windowHeight != fullHeight) {
        targetWidth = windowWidth;
        targetHeight = windowHeight;
    }

    Log("CalculateGameTextureId: Looking for texture with dimensions " + std::to_string(targetWidth) + "x" + std::to_string(targetHeight));

    // Get the maximum texture ID that OpenGL might have allocated
    // We'll check a reasonable range of texture IDs (0-1000)
    const GLuint maxCheckRange = 1000;

    for (GLuint texId = 0; texId < maxCheckRange; texId++) {
        // Check if this is a valid texture object
        if (!glIsTexture(texId)) { continue; }

        // Save current texture binding to restore later
        GLint oldTexture = 0;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);

        // Bind the texture to query its properties
        glBindTexture(GL_TEXTURE_2D, texId);

        // Get texture dimensions
        GLint width = 0, height = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);

        // Check if dimensions match
        if (width == targetWidth && height == targetHeight) {
            // Check texture parameters: minFilter and magFilter must be GL_NEAREST,
            // wrapS and wrapT must be GL_CLAMP_TO_EDGE
            GLint minFilter = 0, magFilter = 0, wrapS = 0, wrapT = 0;
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &magFilter);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrapS);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &wrapT);

            // Restore previous texture binding
            glBindTexture(GL_TEXTURE_2D, oldTexture);

            if (g_gameVersion <= GameVersion(1, 16, 5)) {
                if (minFilter != GL_NEAREST || magFilter != GL_NEAREST || wrapS != GL_CLAMP || wrapT != GL_CLAMP) {
                    Log("CalculateGameTextureId: Texture " + std::to_string(texId) +
                        " has matching dimensions but wrong parameters (minFilter=" + std::to_string(minFilter) + ", magFilter=" +
                        std::to_string(magFilter) + ", wrapS=" + std::to_string(wrapS) + ", wrapT=" + std::to_string(wrapT) + ")");
                    continue; // Skip this texture, try next one
                }
            } else {
                /*
                if (minFilter != GL_NEAREST || magFilter != GL_NEAREST || wrapS != GL_CLAMP || wrapT != GL_CLAMP) {
                    Log("CalculateGameTextureId: Texture " + std::to_string(texId) +
                        " has matching dimensions but wrong parameters (minFilter=" + std::to_string(minFilter) + ", magFilter=" +
                        std::to_string(magFilter) + ", wrapS=" + std::to_string(wrapS) + ", wrapT=" + std::to_string(wrapT) + ")");
                    continue; // Skip this texture, try next one
                }*/
            }

            Log("CalculateGameTextureId: Found matching texture ID " + std::to_string(texId) + " with dimensions " + std::to_string(width) +
                "x" + std::to_string(height));
            return texId;
        }

        // Restore previous texture binding
        glBindTexture(GL_TEXTURE_2D, oldTexture);
    }

    Log("CalculateGameTextureId: No matching texture found in range 1-" + std::to_string(maxCheckRange));
    return UINT_MAX;
}

BOOL WINAPI hkwglDeleteContext(HGLRC hglrc) { return owglDeleteContext(hglrc); }

BOOL WINAPI hkwglSwapBuffers(HDC hDc) {
    auto startTime = std::chrono::high_resolution_clock::now();
    _set_se_translator(SEHTranslator);

    try {
        if (!g_glewLoaded) {
            PROFILE_SCOPE_CAT("GLEW Initialization", "SwapBuffers");
            glewExperimental = GL_TRUE;
            if (glewInit() == GLEW_OK) {
                LogCategory("init", "[RENDER] GLEW Initialized successfully.");
                g_glewLoaded = true;

                // Record the initial context used for sharing.
                g_lastSeenGameGLContext.store(wglGetCurrentContext(), std::memory_order_release);

                // Keep welcome toast system active; per-toast visibility is controlled by config toggles.
                // We still keep touching the legacy "has_opened" marker when GUI is opened.
                g_welcomeToastVisible.store(true);

                CursorTextures::LoadCursorTextures();

                // Initialize shared OpenGL contexts for all worker threads (render, mirror)
                // This must be done BEFORE any thread starts to ensure all contexts are in the same share group
                HGLRC currentContext = wglGetCurrentContext();
                if (currentContext) {
                    if (InitializeSharedContexts(currentContext, hDc)) {
                        LogCategory("init", "[RENDER] Shared contexts initialized - GPU texture sharing enabled for all threads");
                    } else {
                        Log("[RENDER] Shared context initialization failed - starting worker threads in fallback mode");
                    }

                    // ALWAYS start worker threads. They will automatically use the pre-shared contexts if available,
                    // otherwise they fall back to creating/sharing their own contexts.
                    StartRenderThread(currentContext);
                    StartMirrorCaptureThread(currentContext);
                    StartObsHookThread();
                }

                // Aggressively hook glViewport for AMD GPU compatibility
                AttemptAggressiveGlViewportHook();

                // Note: glBlitFramebuffer hook for OBS is now handled by obs_thread.cpp
            } else {
                Log("[RENDER] ERROR: Failed to initialize GLEW.");
                return owglSwapBuffers(hDc);
            }
        }
        if (g_isShuttingDown.load()) { return owglSwapBuffers(hDc); }

        {
            HGLRC currentContext = wglGetCurrentContext();
            HGLRC lastContext = g_lastSeenGameGLContext.load(std::memory_order_acquire);
            if (currentContext && lastContext && currentContext != lastContext) {
                Log("[RENDER] Detected WGL context change - restarting shared contexts/threads");

                StopObsHookThread();
                StopMirrorCaptureThread();
                StopRenderThread();

                CleanupSharedContexts();

                if (InitializeSharedContexts(currentContext, hDc)) {
                    Log("[RENDER] Reinitialized shared contexts after context change");
                } else {
                    Log("[RENDER] Failed to reinitialize shared contexts after context change - restarting threads in fallback mode");
                }

                // Restart worker threads regardless of shared-context init success.
                StartRenderThread(currentContext);
                StartMirrorCaptureThread(currentContext);
                StartObsHookThread();

                // Force recache of game texture IDs in the new context.
                g_cachedGameTextureId.store(UINT_MAX, std::memory_order_release);
                g_lastSeenGameGLContext.store(currentContext, std::memory_order_release);
            } else if (currentContext && (!lastContext)) {
                g_lastSeenGameGLContext.store(currentContext, std::memory_order_release);
            }
        }

        // Start logic thread if not already running (handles OBS detection, hotkey resets, etc.)
        if (!g_logicThreadRunning.load() && g_configLoaded.load()) { StartLogicThread(); }

        // Early exit if config hasn't been loaded yet (prevents race conditions during startup)
        if (!g_configLoaded.load()) { return owglSwapBuffers(hDc); }

        // Grab immutable config snapshot for this frame - all config reads in SwapBuffers use this
        auto frameCfgSnap = GetConfigSnapshot();
        if (!frameCfgSnap) { return owglSwapBuffers(hDc); } // Config not yet published
        const Config& frameCfg = *frameCfgSnap;

        HWND hwnd = WindowFromDC(hDc);
        if (!hwnd) { return owglSwapBuffers(hDc); }
        if (hwnd != g_minecraftHwnd.load()) { g_minecraftHwnd.store(hwnd); }

        // Submit frame capture (GPU-to-GPU copy of the game texture) ONLY when something consumes it.
        // This copy is expensive: it blits the full game texture + inserts fences + flushes.
        // When no mirrors / EyeZoom / OBS / virtual-camera are active, doing this every frame wastes GPU time.
        {
            const bool needCaptureForMirrors = (g_activeMirrorCaptureCount.load(std::memory_order_acquire) > 0);
            const bool needCaptureForEyeZoom = g_showEyeZoom.load(std::memory_order_relaxed) ||
                                               g_isTransitioningFromEyeZoom.load(std::memory_order_relaxed);
            const bool needCaptureForObsOrVc = g_graphicsHookDetected.load(std::memory_order_acquire) || IsVirtualCameraActive();

            const bool needCapture = needCaptureForMirrors || needCaptureForEyeZoom || needCaptureForObsOrVc;
            if (needCapture) {
                GLuint gameTexture = g_cachedGameTextureId.load(std::memory_order_acquire);
                if (gameTexture != UINT_MAX) {
                    ModeViewportInfo viewport = GetCurrentModeViewport();
                    if (viewport.valid) {
                        // Sync screen/game geometry for capture thread to compute render cache.
                        const int fullW_capture = GetCachedScreenWidth();
                        const int fullH_capture = GetCachedScreenHeight();
                        g_captureScreenW.store(fullW_capture, std::memory_order_release);
                        g_captureScreenH.store(fullH_capture, std::memory_order_release);
                        g_captureGameW.store(viewport.width, std::memory_order_release);
                        g_captureGameH.store(viewport.height, std::memory_order_release);

                        // Calculate actual game viewport position (finalX, finalY, finalW, finalH).
                        // Always use stretchX/Y/Width/Height - these contain the actual screen position
                        // whether stretch is enabled (custom position) or disabled (centered).
                        g_captureFinalX.store(viewport.stretchX, std::memory_order_release);
                        g_captureFinalY.store(viewport.stretchY, std::memory_order_release);
                        g_captureFinalW.store(viewport.stretchWidth, std::memory_order_release);
                        g_captureFinalH.store(viewport.stretchHeight, std::memory_order_release);

                        // SubmitFrameCapture already inserts its own fences and flushes after them;
                        // avoid an extra glFlush here (it can reduce FPS by forcing more driver work per frame).
                        SubmitFrameCapture(gameTexture, viewport.width, viewport.height);
                    }
                }
            }
        }

        // Mark safe capture window - capture thread can now safely read the game texture
        g_safeToCapture.store(true, std::memory_order_release);

        // For versions < 1.13.0, always check for window handle changes (fullscreen toggle creates new window)
        // For versions >= 1.13.0, only subclass once
        bool shouldCheckSubclass = (g_gameVersion < GameVersion(1, 13, 0)) || (g_originalWndProc == NULL);

        if (shouldCheckSubclass && hwnd != NULL) {
            PROFILE_SCOPE_CAT("Window Subclassing", "SwapBuffers");
            SubclassGameWindow(hwnd);
        }

        // Render debug texture grid overlay if enabled (BEFORE checking for cached game texture)
        // This allows debugging why game texture caching might be failing
        {
            bool showTextureGrid = frameCfg.debug.showTextureGrid;
            if (showTextureGrid && g_glInitialized && g_solidColorProgram != 0) {
                PROFILE_SCOPE_CAT("Texture Grid Overlay", "Debug");
                ModeViewportInfo viewport = GetCurrentModeViewport();
                RenderTextureGridOverlay(true, viewport.width, viewport.height);
            }
        }

        const int fullW = GetCachedScreenWidth(), fullH = GetCachedScreenHeight();
        bool isFull = IsFullscreen();

        int windowWidth = 0, windowHeight = 0;
        {
            RECT rect;
            if (GetClientRect(hwnd, &rect)) {
                windowWidth = rect.right - rect.left;
                windowHeight = rect.bottom - rect.top;
            }
        }

        if (g_cachedGameTextureId.load() == UINT_MAX) {
            GLint gameTextureId = UINT_MAX;
            {
                PROFILE_SCOPE_CAT("Calculate Game Texture ID", "SwapBuffers");
                gameTextureId = CalculateGameTextureId(windowWidth, windowHeight, fullW, fullH);
            }
            if (gameTextureId != UINT_MAX) {
                g_cachedGameTextureId.store(gameTextureId);
                Log("Calculated game texture ID: " + std::to_string(gameTextureId));
            }
            // If texture not found, leave g_cachedGameTextureId as UINT_MAX.
            // This will retry next frame. We do NOT store UINT_MAX explicitly -
            // it's already UINT_MAX and storing it again would be redundant.
            // Previously this was unconditionally storing UINT_MAX which is fine,
            // but the real fix is that we continue rendering with whatever the
            // render thread has (ready frame / safe read texture fallback) rather
            // than stalling the pipeline.
        }

        // Note: Windows mouse speed application is now handled by the logic thread
        // Note: Hotkey secondary mode reset on world exit is now handled by the logic thread

        if (!isFull) {
            g_safeToCapture.store(false, std::memory_order_release);

            // Determine if we need to use the dual rendering path:
            // - Pre-1.13.0: Always use dual rendering for OBS/virtual camera (centered output)
            // - 1.13.0+: Only use dual rendering for virtual camera (game capture uses backbuffer)
            bool isPre113 = (g_gameVersion < GameVersion(1, 13, 0));
            bool hasObs = g_graphicsHookDetected.load();
            bool hasVirtualCam = IsVirtualCameraActive();

            bool needsProcessedOutput = isPre113 ? (hasObs || hasVirtualCam) : hasVirtualCam;

            if (true) {
                // No OBS/virtual camera active - skip processing entirely

                // Render welcome toast in windowed mode before early return.
                // This is fully self-contained: creates its own shader/VAO/VBO because
                // g_glInitialized / g_imageRenderProgram are not yet initialized at this point
                // (InitializeGPUResources runs after this early-return path).
                // Also uses modern GL (shaders + VAO) because Minecraft 1.17+ uses core profile
                // where fixed-function (glBegin/glEnd) doesn't work.
                // NOTE: Rendering this every frame forever costs GPU time.
                // For performance, show it only briefly after injection / entering windowed mode
                // (and stop once the user opens the GUI).
                static auto s_wt_startTime = std::chrono::steady_clock::now();
                static bool s_wt_prevFullscreen = true;
                const bool isFullNow = IsFullscreen();
                if (!isFullNow && s_wt_prevFullscreen) { s_wt_startTime = std::chrono::steady_clock::now(); }
                s_wt_prevFullscreen = isFullNow;

                const float wtElapsed =
                    std::chrono::duration_cast<std::chrono::duration<float>>(std::chrono::steady_clock::now() - s_wt_startTime).count();
                const bool wantWindowedToast = (wtElapsed <= 10.0f) && !g_showGui.load(std::memory_order_relaxed);

                if (wantWindowedToast && windowWidth > 0 && windowHeight > 0) {
                    // Self-contained GL resources (created lazily, persisted)
                    static GLuint s_wt_program = 0;
                    static GLuint s_wt_vao = 0, s_wt_vbo = 0;
                    static GLint s_wt_locTexture = -1, s_wt_locOpacity = -1;
                    static GLuint s_wt_texture = 0;
                    static int s_wt_texW = 0, s_wt_texH = 0;
                    static bool s_wt_initialized = false;
                    static HGLRC s_wt_lastContext = NULL;

                    // Fullscreen toggles can recreate the game's OpenGL context.
                    // These GL object IDs are context-specific, so force a re-init when HGLRC changes.
                    HGLRC s_wt_currentContext = wglGetCurrentContext();
                    if (s_wt_currentContext != s_wt_lastContext) {
                        s_wt_lastContext = s_wt_currentContext;
                        s_wt_initialized = false;
                        s_wt_program = 0;
                        s_wt_vao = 0;
                        s_wt_vbo = 0;
                        s_wt_locTexture = -1;
                        s_wt_locOpacity = -1;
                        s_wt_texture = 0;
                        s_wt_texW = 0;
                        s_wt_texH = 0;
                    }

                    // Initialize lazily, but be resilient: fullscreen toggles can recreate contexts and
                    // occasionally resource creation can fail transiently. Keep retrying until fully ready.
                    if (!s_wt_initialized) {
                        // Create a minimal shader program
                        const char* vtxSrc = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 TexCoord;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    TexCoord = aTexCoord;
})";
                        const char* fragSrc = R"(#version 330 core
out vec4 FragColor;
in vec2 TexCoord;
uniform sampler2D uTexture;
uniform float uOpacity;
void main() {
    vec4 c = texture(uTexture, TexCoord);
    FragColor = vec4(c.rgb, c.a * uOpacity);
})";
                        if (s_wt_program == 0) {
                            s_wt_program = CreateShaderProgram(vtxSrc, fragSrc);
                            if (s_wt_program) {
                                s_wt_locTexture = glGetUniformLocation(s_wt_program, "uTexture");
                                s_wt_locOpacity = glGetUniformLocation(s_wt_program, "uOpacity");

                                // Set sampler uniform once
                                glUseProgram(s_wt_program);
                                glUniform1i(s_wt_locTexture, 0);
                                glUseProgram(0);
                            }
                        }

                        // Create VAO/VBO (4 floats per vertex: x, y, u, v)
                        if (s_wt_vao == 0) { glGenVertexArrays(1, &s_wt_vao); }
                        if (s_wt_vbo == 0) { glGenBuffers(1, &s_wt_vbo); }
                        if (s_wt_vao && s_wt_vbo) {
                            glBindVertexArray(s_wt_vao);
                            glBindBuffer(GL_ARRAY_BUFFER, s_wt_vbo);
                            glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
                            glEnableVertexAttribArray(0);
                            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
                            glEnableVertexAttribArray(1);
                            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
                            glBindVertexArray(0);
                            glBindBuffer(GL_ARRAY_BUFFER, 0);
                        }

                        // Load toast texture (disable flip for consistent V=0 = top of image)
                        if (s_wt_texture == 0 || s_wt_texW <= 0 || s_wt_texH <= 0) {
                            stbi_set_flip_vertically_on_load_thread(0);

                            HMODULE hModule = NULL;
                            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                               (LPCWSTR)&g_welcomeToastVisible, &hModule);
                            HRSRC hResource = FindResourceW(hModule, MAKEINTRESOURCEW(IDR_TOAST1_PNG), RT_RCDATA);
                            if (hResource) {
                                HGLOBAL hData = LoadResource(hModule, hResource);
                                if (hData) {
                                    DWORD dataSize = SizeofResource(hModule, hResource);
                                    const unsigned char* rawData = (const unsigned char*)LockResource(hData);
                                    if (rawData && dataSize > 0) {
                                        int w, h, channels;
                                        unsigned char* pixels = stbi_load_from_memory(rawData, (int)dataSize, &w, &h, &channels, 4);
                                        if (pixels) {
                                            if (s_wt_texture == 0) { glGenTextures(1, &s_wt_texture); }
                                            glBindTexture(GL_TEXTURE_2D, s_wt_texture);
                                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                                            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                                            glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                                            glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                                            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
                                            glBindTexture(GL_TEXTURE_2D, 0);
                                            s_wt_texW = w;
                                            s_wt_texH = h;
                                            stbi_image_free(pixels);
                                        }
                                    }
                                }
                            }
                        }

                        // Mark initialized only when fully ready
                        s_wt_initialized = (s_wt_program != 0 && s_wt_vao != 0 && s_wt_vbo != 0 && s_wt_texture != 0 && s_wt_texW > 0 &&
                                            s_wt_texH > 0);
                    }

                    if (s_wt_program && s_wt_vao && s_wt_texture && s_wt_texW > 0 && s_wt_texH > 0) {
                        // Save GL state
                        GLint savedProgram, savedVAO, savedVBO, savedFBO, savedTex, savedActiveTex;
                        GLboolean savedBlend, savedDepthTest, savedScissor, savedStencil;
                        GLint savedBlendSrcRGB, savedBlendDstRGB, savedBlendSrcA, savedBlendDstA;
                        GLint savedViewport[4];
                        GLboolean savedColorMask[4];
                        GLint savedUnpackRowLength = 0;
                        GLint savedUnpackSkipPixels = 0;
                        GLint savedUnpackSkipRows = 0;
                        GLint savedUnpackAlignment = 0;

                        glGetIntegerv(GL_CURRENT_PROGRAM, &savedProgram);
                        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &savedVAO);
                        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &savedVBO);
                        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFBO);
                        glGetIntegerv(GL_ACTIVE_TEXTURE, &savedActiveTex);
                        glActiveTexture(GL_TEXTURE0);
                        glGetIntegerv(GL_TEXTURE_BINDING_2D, &savedTex);
                        savedBlend = glIsEnabled(GL_BLEND);
                        savedDepthTest = glIsEnabled(GL_DEPTH_TEST);
                        savedScissor = glIsEnabled(GL_SCISSOR_TEST);
                        savedStencil = glIsEnabled(GL_STENCIL_TEST);
                        glGetIntegerv(GL_BLEND_SRC_RGB, &savedBlendSrcRGB);
                        glGetIntegerv(GL_BLEND_DST_RGB, &savedBlendDstRGB);
                        glGetIntegerv(GL_BLEND_SRC_ALPHA, &savedBlendSrcA);
                        glGetIntegerv(GL_BLEND_DST_ALPHA, &savedBlendDstA);
                        glGetIntegerv(GL_VIEWPORT, savedViewport);
                        glGetBooleanv(GL_COLOR_WRITEMASK, savedColorMask);

                        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &savedUnpackRowLength);
                        glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &savedUnpackSkipPixels);
                        glGetIntegerv(GL_UNPACK_SKIP_ROWS, &savedUnpackSkipRows);
                        glGetIntegerv(GL_UNPACK_ALIGNMENT, &savedUnpackAlignment);

                        // Setup state
                        glBindFramebuffer(GL_FRAMEBUFFER, 0);
                        if (oglViewport)
                            oglViewport(0, 0, windowWidth, windowHeight);
                        else
                            glViewport(0, 0, windowWidth, windowHeight);
                        glDisable(GL_DEPTH_TEST);
                        glDisable(GL_SCISSOR_TEST);
                        glDisable(GL_STENCIL_TEST);
                        glEnable(GL_BLEND);
                        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

                        // Bind shader and resources
                        glUseProgram(s_wt_program);
                        glBindVertexArray(s_wt_vao);
                        glBindBuffer(GL_ARRAY_BUFFER, s_wt_vbo);
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, s_wt_texture);
                        glUniform1f(s_wt_locOpacity, 1.0f);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
                        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
                        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

                        // Scale toast image based on window size (baseline 1080p)
                        float scaleFactor = (static_cast<float>(windowHeight) / 1080.0f) * 0.45f;
                        float drawW = (float)s_wt_texW * scaleFactor;
                        float drawH = (float)s_wt_texH * scaleFactor;

                        // Clicking toast1 switches windowed game into borderless fullscreen.
                        // Detect click edge so this only triggers once per mouse press.
                        static bool s_wt_leftDownLastFrame = false;
                        bool leftDownNow = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
                        if (leftDownNow && !s_wt_leftDownLastFrame) {
                            POINT cursorScreen{};
                            if (GetCursorPos(&cursorScreen)) {
                                POINT cursorClient = cursorScreen;
                                if (ScreenToClient(hwnd, &cursorClient)) {
                                    const bool clickedToast =
                                        cursorClient.x >= 0 && cursorClient.y >= 0 && cursorClient.x < drawW && cursorClient.y < drawH;

                                    if (clickedToast) {
                                        // Multi-monitor support: target the monitor the game window is currently on.
                                        // Use rcMonitor so the window matches the monitor's exact pixel resolution.
                                        // We still keep it acting like a normal window by avoiding WS_POPUP / WS_EX_TOPMOST.
                                        RECT targetRect{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
                                        {
                                            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                                            if (!mon) {
                                                const POINT primaryPt{ 0, 0 };
                                                mon = MonitorFromPoint(primaryPt, MONITOR_DEFAULTTOPRIMARY);
                                            }
                                            if (mon) {
                                                MONITORINFO mi{};
                                                mi.cbSize = sizeof(mi);
                                                if (GetMonitorInfo(mon, &mi)) { targetRect = mi.rcMonitor; }
                                            }
                                        }
                                        const int targetW = (targetRect.right - targetRect.left);
                                        const int targetH = (targetRect.bottom - targetRect.top);

                                        if (IsIconic(hwnd) || IsZoomed(hwnd)) {
                                            // Ensure we're in a normal (restored) state before resizing/restyling.
                                            ShowWindow(hwnd, SW_RESTORE);
                                        }

                                        {
                                            // Keep this as a "window" (avoid WS_POPUP / WS_EX_TOPMOST) so the GPU driver
                                            // doesn't treat it like exclusive/fullscreen, while still removing decorations.
                                            DWORD style = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_STYLE));
                                            style &= ~(WS_POPUP | WS_CAPTION | WS_BORDER | WS_DLGFRAME | WS_THICKFRAME | WS_MINIMIZEBOX |
                                                       WS_MAXIMIZEBOX | WS_SYSMENU);
                                            style |= WS_OVERLAPPED;
                                            SetWindowLongPtr(hwnd, GWL_STYLE, static_cast<LONG_PTR>(style));

                                            DWORD exStyle = static_cast<DWORD>(GetWindowLongPtr(hwnd, GWL_EXSTYLE));
                                            // Clear edge styles so the client area matches the monitor rect exactly.
                                            exStyle &= ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
                                                         WS_EX_STATICEDGE);
                                            exStyle |= WS_EX_APPWINDOW;
                                            SetWindowLongPtr(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(exStyle));
                                        }

                                        SetWindowPos(hwnd, HWND_NOTOPMOST, targetRect.left, targetRect.top, targetW, targetH,
                                                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
                                        g_cachedGameTextureId.store(UINT_MAX);

                                        Log("[TOAST] toast1 clicked - switched to borderless-windowed (current monitor resolution) " +
                                            std::to_string(targetW) + "x" + std::to_string(targetH) + " at (" +
                                            std::to_string(targetRect.left) + "," + std::to_string(targetRect.top) + ")");
                                    }
                                }
                            }
                        }
                        s_wt_leftDownLastFrame = leftDownNow;

                        // Calculate NDC coordinates for top-left placement (no margin)
                        // NDC: (-1,-1) = bottom-left, (+1,+1) = top-right
                        float px1 = 0.0f;
                        float py1 = 0.0f; // pixels from top of window
                        float px2 = drawW;
                        float py2 = drawH;

                        float nx1 = (px1 / windowWidth) * 2.0f - 1.0f;
                        float nx2 = (px2 / windowWidth) * 2.0f - 1.0f;
                        float ny_top = 1.0f - (py1 / windowHeight) * 2.0f; // top edge (high NDC Y)
                        float ny_bot = 1.0f - (py2 / windowHeight) * 2.0f; // bottom edge (low NDC Y)

                        // Vertex data: {ndcX, ndcY, u, v}
                        // No flip: V=0 = top of image, V=1 = bottom of image
                        // ny_top (high) gets V=0 (top of image), ny_bot (low) gets V=1 (bottom)
                        float verts[] = {
                            nx1, ny_bot, 0.0f, 1.0f, // bottom-left  (V=1 = bottom of image)
                            nx2, ny_bot, 1.0f, 1.0f, // bottom-right (V=1)
                            nx2, ny_top, 1.0f, 0.0f, // top-right    (V=0 = top of image)
                            nx1, ny_bot, 0.0f, 1.0f, // bottom-left  (V=1)
                            nx2, ny_top, 1.0f, 0.0f, // top-right    (V=0)
                            nx1, ny_top, 0.0f, 0.0f, // top-left     (V=0)
                        };
                        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
                        glDrawArrays(GL_TRIANGLES, 0, 6);

                        // Restore GL state
                        glUseProgram(savedProgram);
                        glBindVertexArray(savedVAO);
                        glBindBuffer(GL_ARRAY_BUFFER, savedVBO);
                        glBindFramebuffer(GL_FRAMEBUFFER, savedFBO);
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, savedTex);
                        glActiveTexture(savedActiveTex);
                        if (oglViewport)
                            oglViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
                        else
                            glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
                        glColorMask(savedColorMask[0], savedColorMask[1], savedColorMask[2], savedColorMask[3]);

                        glPixelStorei(GL_UNPACK_ROW_LENGTH, savedUnpackRowLength);
                        glPixelStorei(GL_UNPACK_SKIP_PIXELS, savedUnpackSkipPixels);
                        glPixelStorei(GL_UNPACK_SKIP_ROWS, savedUnpackSkipRows);
                        glPixelStorei(GL_UNPACK_ALIGNMENT, savedUnpackAlignment);

                        if (savedBlend)
                            glEnable(GL_BLEND);
                        else
                            glDisable(GL_BLEND);
                        if (savedDepthTest)
                            glEnable(GL_DEPTH_TEST);
                        else
                            glDisable(GL_DEPTH_TEST);
                        if (savedScissor)
                            glEnable(GL_SCISSOR_TEST);
                        else
                            glDisable(GL_SCISSOR_TEST);
                        if (savedStencil)
                            glEnable(GL_STENCIL_TEST);
                        else
                            glDisable(GL_STENCIL_TEST);
                        glBlendFuncSeparate(savedBlendSrcRGB, savedBlendDstRGB, savedBlendSrcA, savedBlendDstA);
                    }
                }
                ClearObsOverride();
                g_obsPre113Windowed.store(false, std::memory_order_release);
                return owglSwapBuffers(hDc);
            }

            // Fall through to dual rendering path
            if (isPre113) {
                // Pre-1.13: Set offset values for OBS blit coordinate remapping (centered output)
                int centeredX = (fullW - windowWidth) / 2;
                int centeredY = (fullH - windowHeight) / 2;
                g_obsPre113Windowed.store(true, std::memory_order_release);
                g_obsPre113OffsetX.store(centeredX, std::memory_order_release);
                g_obsPre113OffsetY.store(centeredY, std::memory_order_release);
                g_obsPre113ContentW.store(windowWidth, std::memory_order_release);
                g_obsPre113ContentH.store(windowHeight, std::memory_order_release);
            } else {
                // 1.13+: No coordinate remapping needed (game renders at window size)
                g_obsPre113Windowed.store(false, std::memory_order_release);
            }
        } else {
            // Fullscreen mode - clear pre-1.13 windowed mode flag
            g_obsPre113Windowed.store(false, std::memory_order_release);
        }

        // Re-enable OBS override when returning to fullscreen (if OBS hook is active)
        // For 1.13+ windowed, OBS should capture directly from backbuffer (no override)
        // For pre-1.13 windowed, OBS needs our centered FBO (enable override)
        if (g_graphicsHookDetected.load()) {
            bool isPre113 = (g_gameVersion < GameVersion(1, 13, 0));
            if (isFull || isPre113) {
                EnableObsOverride();
            } else {
                // 1.13+ windowed: OBS captures backbuffer directly
                ClearObsOverride();
            }
        }

        // return owglSwapBuffers(hDc); // disable here to use NSight for debugging

        if (g_configLoadFailed.load()) {
            g_safeToCapture.store(false, std::memory_order_release);
            HandleConfigLoadFailed(hDc, owglSwapBuffers);
            return owglSwapBuffers(hDc);
        }

        // Lock-free read of current mode ID from double-buffer
        std::string desiredModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];

        // Lock-free read of last frame mode ID from double-buffer
        std::string lastFrameModeIdCopy = g_lastFrameModeIdBuffers[g_lastFrameModeIdIndex.load(std::memory_order_acquire)];

        // Check if mode transition is active (but DON'T update yet - update after rendering
        // so that glViewport hook and RenderModeInternal use the same snapshot values)
        if (IsModeTransitionActive()) {
            g_isTransitioningMode = true;
        } else if (lastFrameModeIdCopy != desiredModeId) {
            // Mode changed but animation already completed or wasn't started
            // This handles cases where SwitchToMode was called and animation is complete
            PROFILE_SCOPE_CAT("Mode Transition Complete", "SwapBuffers");
            g_isTransitioningMode = true;
            Log("Mode transition detected (no animation): " + lastFrameModeIdCopy + " -> " + desiredModeId);

            // Send final WM_SIZE to ensure game has correct dimensions (only in fullscreen mode)
            // In windowed mode, the game manages its own window size - don't override it
            if (isFull) {
                int modeWidth = 0, modeHeight = 0;
                bool modeValid = false;
                {
                    const ModeConfig* newMode = GetMode(desiredModeId);
                    if (newMode) {
                        modeWidth = newMode->width;
                        modeHeight = newMode->height;
                        modeValid = true;
                    }
                }
                if (modeValid) { PostMessage(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(modeWidth, modeHeight)); }
            }
        }

        // Note: Video player update is now done in render_thread

        std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];

        bool showPerformanceOverlay = frameCfg.debug.showPerformanceOverlay;
        bool showProfiler = frameCfg.debug.showProfiler;

        // Enable/disable profiler based on config
        Profiler::GetInstance().SetEnabled(showProfiler);
        if (showProfiler) { Profiler::GetInstance().MarkAsRenderThread(); }

        ModeConfig modeToRenderCopy;
        bool modeFound = false;
        {
            // Use target/desired mode - GetMode is lock-free
            const ModeConfig* tempMode = GetMode(desiredModeId);
            if (!tempMode && g_isTransitioningMode) {
                // Fallback to old mode if new mode not found
                tempMode = GetMode(lastFrameModeIdCopy);
            }
            if (tempMode) {
                modeToRenderCopy = *tempMode;
                modeFound = true;
            }
        }

        // Validate mode before proceeding
        if (!modeFound) {
            Log("ERROR: Could not find mode to render, aborting frame");
            return owglSwapBuffers(hDc);
        }

        bool isEyeZoom = modeToRenderCopy.id == "EyeZoom";
        bool shouldRenderGui = g_showGui.load();

        // Check if we're transitioning FROM EyeZoom
        bool isTransitioningFromEyeZoom = false;
        int eyeZoomAnimatedViewportX = -1; // Animated viewport X for EyeZoom positioning (-1 = use static)

        if (IsModeTransitionActive()) {
            ModeTransitionState eyeZoomTransitionState = GetModeTransitionState();
            std::string fromModeId = eyeZoomTransitionState.fromModeId;

            if (!isEyeZoom && fromModeId == "EyeZoom") {
                // Transitioning FROM EyeZoom - animate out with bounce (follow viewport position)
                isTransitioningFromEyeZoom = true;
                eyeZoomAnimatedViewportX = eyeZoomTransitionState.x;
            } else if (isEyeZoom && fromModeId != "EyeZoom") {
                // Transitioning TO EyeZoom - use animated position during transition in
                eyeZoomAnimatedViewportX = eyeZoomTransitionState.x;
            }
        }

        // Set global GUI state for render thread to pick up
        g_shouldRenderGui.store(shouldRenderGui, std::memory_order_relaxed);
        g_showPerformanceOverlay.store(showPerformanceOverlay, std::memory_order_relaxed);
        g_showProfiler.store(showProfiler, std::memory_order_relaxed);
        // EyeZoom overlay visible when:
        // 1. Target mode is EyeZoom (stable or transitioning TO EyeZoom)
        // 2. Transitioning FROM EyeZoom to another mode (bounce-out animation)
        // EXCEPT: when hideAnimationsInGame is enabled, skip transition-out on user's screen
        bool hideAnimOnScreenEyeZoom = frameCfg.hideAnimationsInGame;
        bool showEyeZoomOnScreen = isEyeZoom || (isTransitioningFromEyeZoom && !hideAnimOnScreenEyeZoom);
        g_showEyeZoom.store(showEyeZoomOnScreen, std::memory_order_relaxed);
        g_eyeZoomFadeOpacity.store(1.0f, std::memory_order_relaxed); // Always full opacity - bounce, not fade
        g_eyeZoomAnimatedViewportX.store(eyeZoomAnimatedViewportX, std::memory_order_relaxed);
        // Release ensures all preceding EyeZoom stores are visible when the reader acquires this value
        g_isTransitioningFromEyeZoom.store(isTransitioningFromEyeZoom, std::memory_order_release);

        // Dual rendering: when OBS hook is detected OR virtual camera is active, render separately for OBS/virtual cam and for user's screen.
        // This must be computed BEFORE any early-exit checks that reference it.
        const bool needsDualRendering = g_graphicsHookDetected.load(std::memory_order_acquire) || IsVirtualCameraActive();

        // PERF: If Toolscreen has nothing to draw and the current mode has no visible effect,
        // skip ALL custom rendering work (including expensive GL state backup).
        // This is critical for games like Minecraft where repeated glGet* calls can stall the driver.
        {
            const bool modeSizesFullscreen = (modeToRenderCopy.width == fullW && modeToRenderCopy.height == fullH);
            const bool stretchIsFullscreen =
                (!modeToRenderCopy.stretch.enabled) ||
                (modeToRenderCopy.stretch.width == fullW && modeToRenderCopy.stretch.height == fullH && modeToRenderCopy.stretch.x == 0 &&
                 modeToRenderCopy.stretch.y == 0);
            const bool borderVisible = modeToRenderCopy.border.enabled && modeToRenderCopy.border.width > 0;

            const bool anyModeOverlaysConfigured =
                (!modeToRenderCopy.mirrorIds.empty() || !modeToRenderCopy.mirrorGroupIds.empty() ||
                 (g_imageOverlaysVisible.load(std::memory_order_acquire) && !modeToRenderCopy.imageIds.empty()) ||
                 (g_windowOverlaysVisible.load(std::memory_order_acquire) && !modeToRenderCopy.windowOverlayIds.empty()));

            const bool anyImGuiOrDebugOverlay = shouldRenderGui || showPerformanceOverlay || showProfiler || showEyeZoomOnScreen ||
                                                frameCfg.debug.showTextureGrid;

            const bool anyOtherCustomOutput = frameCfg.debug.fakeCursor || g_screenshotRequested.load(std::memory_order_relaxed);

            const bool canSkipCustomRender = isFull && !needsDualRendering && !IsModeTransitionActive() && modeSizesFullscreen &&
                                             stretchIsFullscreen && !borderVisible && !anyModeOverlaysConfigured &&
                                             !anyImGuiOrDebugOverlay && !anyOtherCustomOutput;

            if (canSkipCustomRender) {
                // FPS limiter still applies (user expects it), and transition animation still needs to tick.
                // We jump directly to the final SwapBuffers + bookkeeping.

                int targetFPS = frameCfg.fpsLimit;
                if (targetFPS > 0 && g_highResTimer) {
                    PROFILE_SCOPE_CAT("FPS Limit Sleep (Skip Render)", "Timing");

                    const double targetFrameTimeUs = 1000000.0 / targetFPS;
                    const bool isHighFPS = targetFPS > 500;

                    std::lock_guard<std::mutex> lock(g_fpsLimitMutex);
                    auto now = std::chrono::high_resolution_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - g_lastFrameEndTime).count();
                    double timeToWaitUs = targetFrameTimeUs - elapsed;

                    if (timeToWaitUs > 0) {
                        if (isHighFPS) {
                            LARGE_INTEGER dueTime;
                            dueTime.QuadPart = -static_cast<LONGLONG>(timeToWaitUs * 10LL);
                            if (SetWaitableTimer(g_highResTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                                WaitForSingleObject(g_highResTimer, 1000);
                            }
                        } else {
                            if (timeToWaitUs > 10) {
                                LARGE_INTEGER dueTime;
                                dueTime.QuadPart = -static_cast<LONGLONG>(timeToWaitUs * 10LL);
                                if (SetWaitableTimer(g_highResTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                                    WaitForSingleObject(g_highResTimer, 1000);
                                }
                            }
                        }
                        g_lastFrameEndTime = g_lastFrameEndTime + std::chrono::microseconds(static_cast<long long>(targetFrameTimeUs));
                    } else {
                        g_lastFrameEndTime = now;
                    }
                }

                if (IsModeTransitionActive()) {
                    PROFILE_SCOPE_CAT("Mode Transition Animation (Skip Render)", "SwapBuffers");
                    UpdateModeTransition();
                }

                if (frameCfg.debug.delayRenderingUntilFinished) { glFinish(); }
                if (frameCfg.debug.delayRenderingUntilBlitted) { WaitForOverlayBlitFence(); }

                auto swapStartTime = std::chrono::high_resolution_clock::now();
                BOOL result = owglSwapBuffers(hDc);

                g_safeToCapture.store(false, std::memory_order_release);

                auto swapEndTime = std::chrono::high_resolution_clock::now();
                std::chrono::duration<double, std::milli> swapDuration = swapEndTime - swapStartTime;
                g_originalFrameTimeMs = swapDuration.count();

                std::chrono::duration<double, std::milli> fp_ms = swapStartTime - startTime;
                g_lastFrameTimeMs = fp_ms.count();

                {
                    int nextIndex = 1 - g_lastFrameModeIdIndex.load(std::memory_order_relaxed);
                    g_lastFrameModeIdBuffers[nextIndex] = desiredModeId;
                    g_lastFrameModeIdIndex.store(nextIndex, std::memory_order_release);
                    g_lastFrameModeId = desiredModeId;
                }

                return result;
            }
        }

        if (!g_glInitialized) {
            PROFILE_SCOPE_CAT("GPU Resource Init Check", "SwapBuffers");
            Log("[RENDER] Conditions met for GPU resource initialization.");
            InitializeGPUResources();

            if (!g_glInitialized) {
                Log("FATAL: GPU resource initialization failed. Aborting custom render for this frame.");
                g_safeToCapture.store(false, std::memory_order_release);
                return owglSwapBuffers(hDc);
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

        // Note: Image processing is now done in render_thread

        if (g_pendingImageLoad) {
            PROFILE_SCOPE_CAT("Pending Image Load", "SwapBuffers");
            LoadAllImages();
            g_allImagesLoaded = true;
            g_pendingImageLoad = false;
        }

        // Use mode dimensions for game texture sampling, NOT viewport dimensions
        // The viewport may be animated/stretched during mode transitions, but
        // the game texture always remains at the mode's configured width/height
        int current_gameW = modeToRenderCopy.width;
        int current_gameH = modeToRenderCopy.height;

        // Reset OBS capture ready flag each frame - only set true when we have fresh animated content
        // This ensures OBS captures from backbuffer normally when not animating
        g_obsCaptureReady.store(false);

        // needsDualRendering is computed above (before the skip-render fast path)

        // When hideAnimationsInGame is enabled and we're transitioning, skip animation on user's screen
        // (OBS still gets the animated version)
        bool hideAnimOnScreen = frameCfg.hideAnimationsInGame && IsModeTransitionActive();

        {
            PROFILE_SCOPE_CAT("Normal Mode Handling", "Rendering");

            if (needsDualRendering) {
                // Submit animated frame to render thread for OBS capture using helper function
                {
                    PROFILE_SCOPE_CAT("Submit OBS Frame", "OBS");

                    // Build lightweight context struct (no lock-free reads needed here - values already captured)
                    ObsFrameSubmission submission;
                    submission.context.fullW = fullW;
                    submission.context.fullH = fullH;
                    submission.context.gameW = current_gameW;
                    submission.context.gameH = current_gameH;
                    submission.context.gameTextureId = g_cachedGameTextureId.load();
                    submission.context.modeId = modeToRenderCopy.id;
                    submission.context.relativeStretching = modeToRenderCopy.relativeStretching;
                    submission.context.bgR = modeToRenderCopy.background.color.r;
                    submission.context.bgG = modeToRenderCopy.background.color.g;
                    submission.context.bgB = modeToRenderCopy.background.color.b;
                    submission.context.shouldRenderGui = shouldRenderGui;
                    submission.context.showPerformanceOverlay = showPerformanceOverlay;
                    submission.context.showProfiler = showProfiler;
                    submission.context.isEyeZoom = isEyeZoom;
                    submission.context.isTransitioningFromEyeZoom = isTransitioningFromEyeZoom;
                    submission.context.eyeZoomAnimatedViewportX = eyeZoomAnimatedViewportX;
                    submission.context.eyeZoomSnapshotTexture = GetEyeZoomSnapshotTexture();
                    submission.context.eyeZoomSnapshotWidth = GetEyeZoomSnapshotWidth();
                    submission.context.eyeZoomSnapshotHeight = GetEyeZoomSnapshotHeight();
                    submission.context.showTextureGrid = frameCfg.debug.showTextureGrid;
                    submission.context.isWindowed = !isFull;
                    submission.context.isRawWindowedMode = !isFull; // In windowed mode, skip all overlays
                    submission.context.windowW = windowWidth;
                    submission.context.windowH = windowHeight;
                    submission.context.welcomeToastIsFullscreen = isFull;
                    // Always request toast rendering; RenderWelcomeToast() enforces session dismissal for toast2.
                    submission.context.showWelcomeToast = true;
                    submission.isDualRenderingPath = hideAnimOnScreen;

                    // Create fence and flush - these MUST be on GL thread
                    submission.gameTextureFence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    glFlush();

                    // Submit lightweight context - render thread will call BuildObsFrameRequest
                    SubmitObsFrameContext(submission);
                }

                // In windowed mode, skip custom rendering on user's screen
                // The virtual camera gets custom rendering, but game window stays unmodified
                if (isFull) {
                    // Render user view - skip animation only if hideAnimationsInGame is enabled
                    PROFILE_SCOPE_CAT("Render for Screen", "Rendering");
                    RenderMode(&modeToRenderCopy, s, current_gameW, current_gameH, hideAnimOnScreen,
                               false); // hideAnimOnScreen controls animation, false = include onlyOnMyScreen
                }

                // Note: EyeZoom rendering is now done inside RenderModeInternal (before async overlay blit)
            } else {
                // No OBS hook detected - just render for user's screen (only in fullscreen)
                // Still respect hideAnimationsInGame setting (hideAnimOnScreen = hideAnimationsInGame && transitioning)
                if (isFull) { RenderMode(&modeToRenderCopy, s, current_gameW, current_gameH, hideAnimOnScreen, false); }

                // Note: EyeZoom rendering is now done inside RenderModeInternal (before async overlay blit)
            }
        }

        // All ImGui rendering is handled by render thread (via FrameRenderRequest ImGui state fields)
        // Screenshot handling stays on main thread since it needs direct backbuffer access
        if (g_screenshotRequested.exchange(false)) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s.read_fb);
            ScreenshotToClipboard(fullW, fullH);
        }

        // Render fake cursor overlay if enabled (MUST be after RestoreGLState)
        // ^ the above comment might be lying idk
        {
            bool fakeCursorEnabled = frameCfg.debug.fakeCursor;
            if (fakeCursorEnabled) {
                PROFILE_SCOPE_CAT("Fake Cursor Rendering", "Rendering");
                if (IsCursorVisible()) { RenderFakeCursor(hwnd, windowWidth, windowHeight); }
            }
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

        // FPS Limiting Logic - applied before swap buffers
        int targetFPS = 0;
        { targetFPS = frameCfg.fpsLimit; }

        if (targetFPS > 0 && g_highResTimer) {
            PROFILE_SCOPE_CAT("FPS Limit Sleep", "Timing");

            const double targetFrameTimeUs = 1000000.0 / targetFPS; // Target frame time in microseconds
            const bool isHighFPS = targetFPS > 500;                 // Special handling for FPS > 500

            std::lock_guard<std::mutex> lock(g_fpsLimitMutex);

            // Calculate the target time for this frame
            auto targetTime = g_lastFrameEndTime + std::chrono::microseconds(static_cast<long long>(targetFrameTimeUs));
            auto now = std::chrono::high_resolution_clock::now();

            // Check if we're already past the target time (frame took too long)
            if (now < targetTime) {
                // Calculate time to wait in microseconds
                auto timeToWaitUs = std::chrono::duration_cast<std::chrono::microseconds>(targetTime - now).count();

                if (isHighFPS) {
                    if (timeToWaitUs > 1000) {
                        LARGE_INTEGER dueTime;
                        dueTime.QuadPart = -static_cast<LONGLONG>(timeToWaitUs);

                        if (SetWaitableTimer(g_highResTimer, &dueTime, 0, NULL, NULL, FALSE)) {
                            // timeout after 1s in case something goes wrong, we get a hint for debugging
                            WaitForSingleObject(g_highResTimer, 1000);
                        }
                    }
                } else {
                    // Standard behavior for FPS <= 500
                    if (timeToWaitUs > 10) {
                        LARGE_INTEGER dueTime;
                        dueTime.QuadPart = -static_cast<LONGLONG>(timeToWaitUs * 10LL);

                        if (SetWaitableTimer(g_highResTimer, &dueTime, 0, NULL, NULL, FALSE)) { WaitForSingleObject(g_highResTimer, 1000); }
                    }
                }

                // Update to actual target time for consistent pacing
                g_lastFrameEndTime = targetTime;
            } else {
                // Frame took longer than target - reset to current time
                g_lastFrameEndTime = now;
            }
        }

        // Update mode transition animation AFTER all rendering is complete
        // This ensures glViewport hook and RenderModeInternal use the same snapshot values,
        // preventing the 1-frame desync that caused black gaps between background and game
        if (IsModeTransitionActive()) {
            PROFILE_SCOPE_CAT("Mode Transition Animation", "SwapBuffers");
            UpdateModeTransition();
        }

        // Optionally wait for all GPU rendering to complete before SwapBuffers
        if (frameCfg.debug.delayRenderingUntilFinished) { glFinish(); }

        // Optionally wait for the async overlay blit fence to complete before SwapBuffers
        if (frameCfg.debug.delayRenderingUntilBlitted) { WaitForOverlayBlitFence(); }

        auto swapStartTime = std::chrono::high_resolution_clock::now();
        BOOL result = owglSwapBuffers(hDc);

        // End safe capture window - next frame will start rendering soon
        g_safeToCapture.store(false, std::memory_order_release);

        auto swapEndTime = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> swapDuration = swapEndTime - swapStartTime;
        g_originalFrameTimeMs = swapDuration.count();

        // Calculate overhead time (total time minus actual swap buffers time)
        std::chrono::duration<double, std::milli> fp_ms = swapStartTime - startTime;
        g_lastFrameTimeMs = fp_ms.count();

        // Update last frame mode ID for next frame's viewport calculations (lock-free)
        {
            int nextIndex = 1 - g_lastFrameModeIdIndex.load(std::memory_order_relaxed);
            g_lastFrameModeIdBuffers[nextIndex] = desiredModeId;
            g_lastFrameModeIdIndex.store(nextIndex, std::memory_order_release);
            g_lastFrameModeId = desiredModeId; // Keep legacy variable in sync
        }

        return result;
    } catch (const SE_Exception& e) {
        LogException("hkwglSwapBuffers (SEH)", e.getCode(), e.getInfo());
        return owglSwapBuffers(hDc);
    } catch (const std::exception& e) {
        LogException("hkwglSwapBuffers", e);
        return owglSwapBuffers(hDc);
    } catch (...) {
        Log("FATAL UNKNOWN EXCEPTION in hkwglSwapBuffers!");
        return owglSwapBuffers(hDc);
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)DllMain,
                           &g_hModule);

        // Install global exception handlers FIRST (before anything else can throw)
        InstallGlobalExceptionHandlers();

        // Verify logging works immediately
        LogCategory("init", "========================================");
        LogCategory("init", "=== Toolscreen INITIALIZATION START ===");
        LogCategory("init", "========================================");
        PrintVersionToStdout();

        // Create high-resolution waitable timer for FPS limiting (Windows 10 1803+)
        g_highResTimer = CreateWaitableTimerExW(NULL,                                  // Default security
                                                NULL,                                  // No name
                                                CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, // High-resolution flag
                                                TIMER_ALL_ACCESS                       // Full access
        );
        if (g_highResTimer) {
            LogCategory("init", "High-resolution waitable timer created successfully for FPS limiting.");
        } else {
            Log("Warning: Failed to create high-resolution waitable timer. FPS limiting may be less precise.");
        }

        g_toolscreenPath = GetToolscreenPath();
        if (!g_toolscreenPath.empty()) {
            // Create logs subdirectory
            std::wstring logsDir = g_toolscreenPath + L"\\logs";
            CreateDirectoryW(logsDir.c_str(), NULL);

            // Path to latest.log
            std::wstring latestLogPath = logsDir + L"\\latest.log";

            // If latest.log exists, rename it to timestamped filename
            if (GetFileAttributesW(latestLogPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                // Get file last write time (not creation time - creation time stays the same
                // across sessions, causing archived logs to have incorrect/stale dates)
                HANDLE hFile =
                    CreateFileW(latestLogPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile != INVALID_HANDLE_VALUE) {
                    FILETIME lastWriteTime;
                    if (GetFileTime(hFile, NULL, NULL, &lastWriteTime)) {
                        // Convert to local time first, then to system time
                        // This ensures the timestamp reflects the user's timezone
                        FILETIME localFileTime;
                        FileTimeToLocalFileTime(&lastWriteTime, &localFileTime);
                        SYSTEMTIME st;
                        FileTimeToSystemTime(&localFileTime, &st);

                        // Format: YYYYMMDD_HHMMSS
                        WCHAR timestamp[32];
                        swprintf_s(timestamp, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

                        std::wstring archivedLogPath = logsDir + L"\\" + timestamp + L".log";

                        // Close handle before moving
                        CloseHandle(hFile);

                        // Check if archive path already exists (same second collision)
                        // If so, append a counter
                        if (GetFileAttributesW(archivedLogPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                            for (int counter = 1; counter < 100; counter++) {
                                std::wstring altPath = logsDir + L"\\" + timestamp + L"_" + std::to_wstring(counter) + L".log";
                                if (GetFileAttributesW(altPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                                    archivedLogPath = altPath;
                                    break;
                                }
                            }
                        }

                        // Rename the file
                        if (!MoveFileW(latestLogPath.c_str(), archivedLogPath.c_str())) {
                            // If rename fails, DON'T delete - log a warning and the file will be
                            // overwritten when the new latest.log is opened (data preserved until then)
                            // This is better than losing all the log data
                            Log("WARNING: Could not rename old log to " + WideToUtf8(archivedLogPath) +
                                ", error code: " + std::to_string(GetLastError()));
                        } else {
                            // Compress the archived log to .gz on a background thread
                            // so we don't block DLL initialization
                            std::wstring archiveSrc = archivedLogPath;
                            std::thread([archiveSrc]() {
                                std::wstring gzPath = archiveSrc + L".gz";
                                if (CompressFileToGzip(archiveSrc, gzPath)) {
                                    // Compression succeeded - delete the uncompressed file
                                    DeleteFileW(archiveSrc.c_str());
                                }
                                // If compression fails, keep the uncompressed .log as fallback
                            }).detach();
                        }
                    } else {
                        CloseHandle(hFile);
                    }
                }
            }
            // Note: If latest.log doesn't exist, that's fine - this is normal for first run

            // Open new latest.log
            {
                std::lock_guard<std::mutex> lock(g_logFileMutex);
                logFile.open(latestLogPath, std::ios_base::out | std::ios_base::trunc);
            }

            // Start async logging thread now that log file is open
            StartLogThread();

            g_modeFilePath = g_toolscreenPath + L"\\mode.txt";
        }
        LogCategory("init", "--- DLL instance attached ---");
        LogVersionInfo(); // Log version information
        if (g_toolscreenPath.empty()) { Log("FATAL: Could not get toolscreen directory."); }

        // Detect game version from command line arguments
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
            // No version detected - enable hook by default for backward compatibility
            LogCategory("init", "No game version detected from command line.");
        }

        LoadConfig();

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
        // can cause null-pointer crashes in CRT functions like std::stringstream formatting.
        g_monitorThread = std::thread([]() { FileMonitorThread(nullptr); });
        g_imageMonitorThread = std::thread([]() { ImageMonitorThread(nullptr); });

        StartWindowCaptureThread();

        if (MH_Initialize() != MH_OK) {
            Log("ERROR: MH_Initialize() failed!");
            return TRUE;
        }

        LogCategory("init", "Setting up hooks...");

        // Get function addresses
        HMODULE hOpenGL32 = GetModuleHandle(L"opengl32.dll");
        HMODULE hUser32 = GetModuleHandle(L"user32.dll");
        HMODULE hGlfw = GetModuleHandle(L"glfw.dll");

// Create all hooks
#define HOOK(mod, name) CreateHookOrDie(GetProcAddress(mod, #name), &hk##name, &o##name, #name)
        HOOK(hOpenGL32, wglSwapBuffers);
        HOOK(hOpenGL32, wglDeleteContext);
        if (IsVersionInRange(g_gameVersion, GameVersion(1, 0, 0), GameVersion(1, 21, 0))) {
            if (HOOK(hOpenGL32, glViewport)) {
                g_glViewportHookCount.fetch_add(1);
                LogCategory("init", "Initial glViewport hook created via opengl32.dll");
            }
        }
        HOOK(hOpenGL32, glClear);
        HOOK(hUser32, SetCursorPos);
        HOOK(hUser32, ClipCursor);
        HOOK(hUser32, SetCursor);
        HOOK(hUser32, GetRawInputData);
        HOOK(hGlfw, glfwSetInputMode);
#undef HOOK

        // glBlitNamedFramebuffer is an extension, try to hook it but don't fail if unavailable
        LPVOID pGlBlitNamedFramebuffer = GetProcAddress(hOpenGL32, "glBlitNamedFramebuffer");
        if (pGlBlitNamedFramebuffer != NULL) {
            CreateHookOrDie(pGlBlitNamedFramebuffer, &hkglBlitNamedFramebuffer, &oglBlitNamedFramebuffer, "glBlitNamedFramebuffer");
        } else {
            LogCategory("init",
                        "WARNING: glBlitNamedFramebuffer not found in opengl32.dll - will attempt to hook via GLEW after context init");
        }

        if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) {
            Log("ERROR: MH_EnableHook(MH_ALL_HOOKS) failed!");
            return TRUE;
        }

        LogCategory("init", "Hooks enabled.");

        // Save the original Windows mouse speed so we can restore it on exit
        SaveOriginalWindowsMouseSpeed();

        // Save the original key repeat settings so we can restore them on exit
        SaveOriginalKeyRepeatSettings();

        // Immediately apply loaded key repeat settings to the system.
        ApplyKeyRepeatSettings();

    } else if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        // CRITICAL: When DLL_PROCESS_DETACH is called, the process may be terminating
        // We should do MINIMAL cleanup here. Windows will automatically clean up:
        // - Memory allocations
        // - GPU resources (driver handles cleanup)
        // - Thread handles
        // Trying to do too much cleanup can cause crashes because:
        // 1. Other threads may still be running
        // 2. The game may still be making OpenGL calls
        // 3. Disabling hooks can corrupt the game's state

        g_isShuttingDown = true;
        Log("DLL Detached. Performing minimal cleanup...");

        // Close high-resolution timer
        if (g_highResTimer) {
            CloseHandle(g_highResTimer);
            g_highResTimer = NULL;
        }

        // ONLY save config and stop our own threads
        // Do NOT touch hooks, GPU resources, or game state

        // Restore original Windows mouse speed before exiting
        RestoreWindowsMouseSpeed();

        // Restore original key repeat settings before exiting
        RestoreKeyRepeatSettings();

        SaveConfigImmediate();
        Log("Config saved.");

        // Stop monitoring threads
        g_stopMonitoring = true;
        if (g_monitorThread.joinable()) { g_monitorThread.join(); }

        g_stopImageMonitoring = true;
        if (g_imageMonitorThread.joinable()) { g_imageMonitorThread.join(); }

        // Stop background threads
        StopWindowCaptureThread();

        // Cleanup shared OpenGL contexts
        CleanupSharedContexts();

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
        // - Disable hooks (causes game to crash)
        // - Delete GPU resources (Windows/driver handles this)
        // - Restore window procedure (game might still use it during shutdown)
        //   Note: Even if we wanted to restore it, the window may already be destroyed (especially < 1.13.0)
        // - Call GL functions (context may be invalid)
        // - Uninitialize MinHook (can corrupt game state)

        // Final log and close
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
    }
    return TRUE;
}