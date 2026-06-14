#include "logic_thread.h"
#include "common/mode_dimensions.h"
#include "gui/gui.h"
#include "render/mirror_thread.h"
#include "common/profiler.h"
#include "render/render.h"
#include "common/utils.h"
#include "version.h"
#include "hooks/input_hook.h"
#include <Windows.h>
#include <unordered_map>
#include <unordered_set>
#include <thread>

bool PublishConfigSnapshotIfUnchanged(const std::shared_ptr<const Config>& expectedSnapshot, const Config& config);

std::atomic<bool> g_logicThreadRunning{ false };
static std::thread g_logicThread;
static std::atomic<bool> g_logicThreadShouldStop{ false };

extern std::atomic<bool> g_graphicsHookDetected;
extern std::atomic<HMODULE> g_graphicsHookModule;
extern std::chrono::steady_clock::time_point g_lastGraphicsHookCheck;
extern const int GRAPHICS_HOOK_CHECK_INTERVAL_MS;

extern std::atomic<HWND> g_minecraftHwnd;
extern std::atomic<bool> g_configLoaded;

extern std::string g_gameStateBuffers[2];
extern std::atomic<int> g_currentGameStateIndex;

extern std::atomic<bool> g_windowsMouseSpeedApplied;
extern int g_originalWindowsMouseSpeed;

extern std::atomic<bool> g_isShuttingDown;
extern WNDPROC g_originalWndProc;
extern std::atomic<bool> g_gameWindowActive;

extern PendingModeSwitch g_pendingModeSwitch;
extern std::mutex g_pendingModeSwitchMutex;

extern GameVersion g_gameVersion;

void ApplyWindowsMouseSpeed();

// Double-buffered viewport cache for lock-free access by hkglViewport
CachedModeViewport g_viewportModeCache[2];
std::atomic<int> g_viewportModeCacheIndex{ 0 };
static std::string s_lastCachedModeId;
static uint64_t s_lastCachedViewportSnapshotVersion = 0;

static bool s_wasInWorld = false;
static int s_lastAppliedWindowsMouseSpeed = -1;
static std::string s_previousGameStateForReset = "init";

static std::atomic<int> s_cachedScreenWidth{ 0 };
static std::atomic<int> s_cachedScreenHeight{ 0 };

// - Periodic refresh is a safety net in case move messages are missed.
// - If another thread detects a size change and updates the cache, it requests
//   an expression-dimension recalculation which MUST occur on the logic thread.
static std::atomic<bool> s_screenMetricsDirty{ true };
static std::atomic<bool> s_screenMetricsRecalcRequested{ false };
static std::atomic<ULONGLONG> s_lastScreenMetricsRefreshMs{ 0 };
static std::atomic<ULONGLONG> s_lastLogicThreadScreenMetricsProbeMs{ 0 };
static std::atomic<bool> s_startupMetricsResyncPending{ true };

static void ComputeScreenMetricsForGameWindow(int& outW, int& outH) {
    constexpr int kInactiveTransientClientMin = 32;
    outW = 0;
    outH = 0;

    HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
    if (hwnd && IsWindow(hwnd)) {
        RECT clientRect{};
        if (GetClientRect(hwnd, &clientRect)) {
            const int clientW = clientRect.right - clientRect.left;
            const int clientH = clientRect.bottom - clientRect.top;
            if (clientW > 0 && clientH > 0) {
                const bool windowActive = g_gameWindowActive.load(std::memory_order_relaxed);
                const bool isInactiveTinySize = !windowActive && (clientW < kInactiveTransientClientMin || clientH < kInactiveTransientClientMin);
                if (isInactiveTinySize) { return; }
                outW = clientW;
                outH = clientH;
                return;
            }
        }

        // Important: do NOT fall back to monitor metrics while a valid game window exists.
        // During startup/minimize/live-resize, client size may be transiently zero; using
        // monitor size in that window corrupts relative mode sizing calculations.
        return;
    }

    // Only when there is no valid game window yet, fall back to monitor/system size
    // so early startup code can still proceed.
    if (!GetMonitorSizeForWindow(hwnd, outW, outH)) {
        outW = GetSystemMetrics(SM_CXSCREEN);
        outH = GetSystemMetrics(SM_CYSCREEN);
    }
}

static bool RefreshCachedScreenMetricsIfNeeded(bool requestRecalcOnChange) {
    constexpr ULONGLONG kPeriodicRefreshMs = 250; // fast enough to catch monitor moves, cheap enough for render thread callers
    ULONGLONG now = GetTickCount64();

    bool forced = s_screenMetricsDirty.exchange(false, std::memory_order_relaxed);
    ULONGLONG last = s_lastScreenMetricsRefreshMs.load(std::memory_order_relaxed);
    bool periodic = (now - last) >= kPeriodicRefreshMs;

    if (!forced && !periodic) { return false; }
    s_lastScreenMetricsRefreshMs.store(now, std::memory_order_relaxed);

    int newW = 0, newH = 0;
    ComputeScreenMetricsForGameWindow(newW, newH);
    if (newW <= 0 || newH <= 0) {
        // If a game window exists but currently reports an invalid/zero client area
        // (common during startup/minimize/live-resize), invalidate cached dimensions.
        // This prevents stale monitor-sized values from being reused for relative-mode math.
        HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
        if (hwnd && IsWindow(hwnd)) {
            if (!g_gameWindowActive.load(std::memory_order_relaxed)) {
                // While inactive, transient tiny/invalid client sizes are expected during alt-tab.
                // Keep last valid metrics to avoid collapsing mode geometry for OBS/virtual camera.
                return false;
            }
            int prevW = s_cachedScreenWidth.load(std::memory_order_relaxed);
            int prevH = s_cachedScreenHeight.load(std::memory_order_relaxed);
            if (prevW != 0 || prevH != 0) {
                s_cachedScreenWidth.store(0, std::memory_order_relaxed);
                s_cachedScreenHeight.store(0, std::memory_order_relaxed);
            }
        }
        return false;
    }

    int prevW = s_cachedScreenWidth.load(std::memory_order_relaxed);
    int prevH = s_cachedScreenHeight.load(std::memory_order_relaxed);

    if (prevW != newW || prevH != newH) {
        s_cachedScreenWidth.store(newW, std::memory_order_relaxed);
        s_cachedScreenHeight.store(newH, std::memory_order_relaxed);

        if (requestRecalcOnChange) { s_screenMetricsRecalcRequested.store(true, std::memory_order_relaxed); }
        return true;
    }

    return false;
}

void InvalidateCachedScreenMetrics() {
    s_screenMetricsDirty.store(true, std::memory_order_relaxed);
}

void RequestScreenMetricsRecalculation() {
    s_screenMetricsDirty.store(true, std::memory_order_relaxed);
    s_screenMetricsRecalcRequested.store(true, std::memory_order_relaxed);
}

static std::vector<std::string> s_lastActiveMirrorIds;
static std::string s_lastMirrorConfigModeId;
static uint64_t s_lastMirrorConfigSnapshotVersion = 0;
static int s_lastViewportScreenW = 0;
static int s_lastViewportScreenH = 0;

void UpdateActiveMirrorConfigs() {
    PROFILE_SCOPE_CAT("LT Mirror Configs", "Logic Thread");

    struct MirrorGroupPlacement {
        const MirrorGroupConfig* group = nullptr;
        const MirrorGroupItem* item = nullptr;
    };

    // Use config snapshot for thread-safe access to modes/mirrors/mirrorGroups
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return;
    const Config& cfg = *cfgSnap;

    const uint64_t snapVer = g_configSnapshotVersion.load(std::memory_order_acquire);
    static uint64_t s_lookupSnapshotVersion = 0;
    static std::unordered_map<std::string, const MirrorGroupConfig*> s_groupByName;
    static std::unordered_map<std::string, const MirrorConfig*> s_mirrorByName;

    if (s_lookupSnapshotVersion != snapVer) {
        s_lookupSnapshotVersion = snapVer;
        s_groupByName.clear();
        s_mirrorByName.clear();
        s_groupByName.reserve(cfg.mirrorGroups.size());
        s_mirrorByName.reserve(cfg.mirrors.size());
        for (const auto& group : cfg.mirrorGroups) { s_groupByName[group.name] = &group; }
        for (const auto& mirror : cfg.mirrors) { s_mirrorByName[mirror.name] = &mirror; }
    }

    // Get current mode ID from double-buffer (lock-free)
    std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];

    if (currentModeId == s_lastMirrorConfigModeId && snapVer == s_lastMirrorConfigSnapshotVersion) {
        return;
    }
    const ModeConfig* mode = GetModeFromSnapshotOrFallback(cfg, currentModeId);
    if (!mode) { return; }

    std::vector<std::string> currentMirrorIds;
    CollectModeOrderedMirrorIds(cfg, *mode, currentMirrorIds);
    std::unordered_map<std::string, MirrorGroupPlacement> mirrorPlacements;
    mirrorPlacements.reserve(currentMirrorIds.size() + 4);

    for (const auto& source : mode->sources) {
        if (source.type != ModeSourceType::MirrorGroup) continue;

        auto groupIt = s_groupByName.find(source.id);
        if (groupIt == s_groupByName.end() || !groupIt->second) {
            continue;
        }

        const MirrorGroupConfig& group = *groupIt->second;
        for (const auto& item : group.mirrors) {
            if (!item.enabled) continue;
            mirrorPlacements.try_emplace(item.mirrorId, MirrorGroupPlacement{ &group, &item });
        }
    }

    std::vector<MirrorConfig> activeMirrorsForCapture;
    activeMirrorsForCapture.reserve(currentMirrorIds.size());
    for (const auto& mirrorId : currentMirrorIds) {
        auto mirrorIt = s_mirrorByName.find(mirrorId);
        if (mirrorIt == s_mirrorByName.end() || !mirrorIt->second) {
            continue;
        }

        const MirrorConfig& mirror = *mirrorIt->second;
        MirrorConfig activeMirror = mirror;

        auto placementIt = mirrorPlacements.find(mirrorId);
        if (placementIt != mirrorPlacements.end() && placementIt->second.group && placementIt->second.item) {
            const MirrorGroupConfig& group = *placementIt->second.group;
            const MirrorGroupItem& item = *placementIt->second.item;

            int groupX = group.output.x;
            int groupY = group.output.y;
            if (group.output.useRelativePosition) {
                int screenW = GetCachedWindowWidth();
                int screenH = GetCachedWindowHeight();
                groupX = static_cast<int>(group.output.relativeX * screenW);
                groupY = static_cast<int>(group.output.relativeY * screenH);
            }
            activeMirror.output.x = groupX + item.offsetX;
            activeMirror.output.y = groupY + item.offsetY;
            activeMirror.output.relativeTo = group.output.relativeTo;
            activeMirror.output.useRelativePosition = group.output.useRelativePosition;
            activeMirror.output.relativeX = group.output.relativeX;
            activeMirror.output.relativeY = group.output.relativeY;
            if (item.widthPercent != 1.0f || item.heightPercent != 1.0f) {
                activeMirror.output.separateScale = true;
                float baseScaleX = mirror.output.separateScale ? mirror.output.scaleX : mirror.output.scale;
                float baseScaleY = mirror.output.separateScale ? mirror.output.scaleY : mirror.output.scale;
                activeMirror.output.scaleX = baseScaleX * item.widthPercent;
                activeMirror.output.scaleY = baseScaleY * item.heightPercent;
            }
        }

        activeMirrorsForCapture.push_back(activeMirror);
    }
    UpdateMirrorCaptureConfigs(activeMirrorsForCapture);
    s_lastActiveMirrorIds = currentMirrorIds;

    s_lastMirrorConfigModeId = currentModeId;
    s_lastMirrorConfigSnapshotVersion = snapVer;
}

void UpdateCachedScreenMetrics() {
    PROFILE_SCOPE_CAT("LT Screen Metrics", "Logic Thread");

    constexpr ULONGLONG kLogicThreadScreenMetricsProbeMs = 100;
    const ULONGLONG nowMs = GetTickCount64();
    const ULONGLONG lastProbeMs = s_lastLogicThreadScreenMetricsProbeMs.load(std::memory_order_relaxed);
    if ((nowMs - lastProbeMs) >= kLogicThreadScreenMetricsProbeMs) {
        s_lastLogicThreadScreenMetricsProbeMs.store(nowMs, std::memory_order_relaxed);
        s_screenMetricsDirty.store(true, std::memory_order_relaxed);
    }

    const bool startupPending = s_startupMetricsResyncPending.load(std::memory_order_relaxed);
    bool startupClientReady = false;
    if (startupPending) {
        HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
        if (hwnd) {
            RECT clientRect{};
            if (GetClientRect(hwnd, &clientRect)) {
                startupClientReady = (clientRect.right - clientRect.left) > 0 && (clientRect.bottom - clientRect.top) > 0;
            }
        }

        // Keep requesting refresh/recalc until the client area is valid at least once.
        s_screenMetricsDirty.store(true, std::memory_order_relaxed);
        if (!startupClientReady) {
            s_screenMetricsRecalcRequested.store(true, std::memory_order_relaxed);
        }
    }

    // Note: other threads may refresh the cache (to avoid returning stale values),
    int prevWidth = s_cachedScreenWidth.load(std::memory_order_relaxed);
    int prevHeight = s_cachedScreenHeight.load(std::memory_order_relaxed);

    bool changed = RefreshCachedScreenMetricsIfNeeded(/*requestRecalcOnChange=*/false);
    bool recalcRequested = s_screenMetricsRecalcRequested.exchange(false, std::memory_order_relaxed);

    int newWidth = s_cachedScreenWidth.load(std::memory_order_relaxed);
    int newHeight = s_cachedScreenHeight.load(std::memory_order_relaxed);

    // Recalculate expression/relative-based dimensions if screen size changed
    // or if another thread explicitly requested it.
    // Require only that current metrics are valid so resize after startup/minimize still triggers recalculation.
    const bool startupShouldRunNow = startupPending && startupClientReady;
    if (newWidth > 0 && newHeight > 0 && (startupShouldRunNow || changed || recalcRequested || prevWidth != newWidth || prevHeight != newHeight)) {
        auto baseSnapshot = GetConfigSnapshot();
        if (!baseSnapshot) { return; }

        std::string currentModeId = GetPublishedCurrentModeId();
        int beforeModeW = 0;
        int beforeModeH = 0;
        if (const ModeConfig* currentModeBefore = GetModeFromSnapshotOrFallback(*baseSnapshot, currentModeId)) {
            beforeModeW = currentModeBefore->width;
            beforeModeH = currentModeBefore->height;
        }

        Config resolvedConfig = *baseSnapshot;
        RecalculateModeDimensions(resolvedConfig, newWidth, newHeight);

        int afterModeW = 0;
        int afterModeH = 0;
        if (const ModeConfig* currentModeAfter = GetModeFromSnapshotOrFallback(resolvedConfig, currentModeId)) {
            afterModeW = currentModeAfter->width;
            afterModeH = currentModeAfter->height;
        }

        const bool modeSizeChanged = (afterModeW != beforeModeW || afterModeH != beforeModeH);
        bool clientSizeDiffersFromMode = false;
        {
            HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
            if (hwnd && afterModeW > 0 && afterModeH > 0) {
                RECT clientRect{};
                if (GetClientRect(hwnd, &clientRect)) {
                    const int clientW = clientRect.right - clientRect.left;
                    const int clientH = clientRect.bottom - clientRect.top;
                    if (clientW > 0 && clientH > 0) {
                        clientSizeDiffersFromMode = (clientW != afterModeW) || (clientH != afterModeH);
                    }
                }
            }
        }

        const bool fullscreenStretchMode = EqualsIgnoreCase(currentModeId, "Fullscreen");
        const bool shouldEnforceForExternalResize = clientSizeDiffersFromMode;
        const bool shouldSendStartupWmSize = startupShouldRunNow;
        const bool shouldSendFullscreenModeSize = fullscreenStretchMode && modeSizeChanged;
        const bool shouldEnforceModeSize = !fullscreenStretchMode && (modeSizeChanged || shouldEnforceForExternalResize);
        const bool shouldSendWmSize = shouldSendStartupWmSize || shouldSendFullscreenModeSize || shouldEnforceModeSize;

        const bool sourceSnapshotStillCurrent = PublishConfigSnapshotIfUnchanged(baseSnapshot, resolvedConfig);
        const bool sameModeStillActive = (GetPublishedCurrentModeId() == currentModeId);

        if (sourceSnapshotStillCurrent && sameModeStillActive && afterModeW > 0 && afterModeH > 0 && shouldSendWmSize &&
            IsResolutionChangeSupported(g_gameVersion)) {
            HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
            if (hwnd) { RequestWindowClientResize(hwnd, afterModeW, afterModeH, "logic_thread:screen_metrics"); }
        }

        if (sourceSnapshotStillCurrent) {
            const std::string activeModeIdAfterPublish = GetPublishedCurrentModeId();
            if (const ModeConfig* activeMode = GetModeFromSnapshotOrFallback(resolvedConfig, activeModeIdAfterPublish)) {
                RetargetActiveModeTransition(*activeMode);
            }
        } else {
            // A concurrent publish won the snapshot race. Retry next tick against
            // the newer config so resize-driven mode dimensions do not stay stale.
            s_screenMetricsRecalcRequested.store(true, std::memory_order_relaxed);
        }

        if (startupShouldRunNow) { s_startupMetricsResyncPending.store(false, std::memory_order_relaxed); }
    }
}

int GetCachedWindowWidth() {
    RefreshCachedScreenMetricsIfNeeded(/*requestRecalcOnChange=*/true);

    int w = s_cachedScreenWidth.load(std::memory_order_relaxed);
    if (w == 0) {
        // Startup fallback if logic thread hasn't populated the cache yet.
        int tmpW = 0, tmpH = 0;
        ComputeScreenMetricsForGameWindow(tmpW, tmpH);
        if (tmpW > 0) {
            s_cachedScreenWidth.store(tmpW, std::memory_order_relaxed);
            s_cachedScreenHeight.store(tmpH, std::memory_order_relaxed);
            w = tmpW;
        }
    }
    return w;
}

int GetCachedWindowHeight() {
    RefreshCachedScreenMetricsIfNeeded(/*requestRecalcOnChange=*/true);

    int h = s_cachedScreenHeight.load(std::memory_order_relaxed);
    if (h == 0) {
        // Startup fallback if logic thread hasn't populated the cache yet.
        int tmpW = 0, tmpH = 0;
        ComputeScreenMetricsForGameWindow(tmpW, tmpH);
        if (tmpH > 0) {
            s_cachedScreenWidth.store(tmpW, std::memory_order_relaxed);
            s_cachedScreenHeight.store(tmpH, std::memory_order_relaxed);
            h = tmpH;
        }
    }
    return h;
}

void UpdateCachedWindowMetricsFromSize(int clientWidth, int clientHeight) {
    constexpr int kInactiveTransientClientMin = 32;
    if (clientWidth <= 0 || clientHeight <= 0) { return; }
    if (!g_gameWindowActive.load(std::memory_order_relaxed) &&
        (clientWidth < kInactiveTransientClientMin || clientHeight < kInactiveTransientClientMin)) {
        return;
    }

    const int prevW = s_cachedScreenWidth.load(std::memory_order_relaxed);
    const int prevH = s_cachedScreenHeight.load(std::memory_order_relaxed);

    if (prevW != clientWidth || prevH != clientHeight) {
        s_cachedScreenWidth.store(clientWidth, std::memory_order_relaxed);
        s_cachedScreenHeight.store(clientHeight, std::memory_order_relaxed);
        s_screenMetricsRecalcRequested.store(true, std::memory_order_relaxed);
    }

    s_lastScreenMetricsRefreshMs.store(GetTickCount64(), std::memory_order_relaxed);
    s_screenMetricsDirty.store(false, std::memory_order_relaxed);
}

void UpdateCachedViewportMode() {
    PROFILE_SCOPE_CAT("LT Viewport Cache", "Logic Thread");

    // Read current mode ID from double-buffer (lock-free)
    std::string currentModeId = g_modeIdBuffers[g_currentModeIdIndex.load(std::memory_order_acquire)];
    const uint64_t snapVer = g_configSnapshotVersion.load(std::memory_order_acquire);

    // Also force periodic refresh every 60 ticks (~1 second) as a safety net
    static int s_ticksSinceRefresh = 0;
    bool guiOpen = g_showGui.load(std::memory_order_relaxed);
    bool periodicRefresh = (++s_ticksSinceRefresh >= 60);
    const int screenW = s_cachedScreenWidth.load(std::memory_order_relaxed);
    const int screenH = s_cachedScreenHeight.load(std::memory_order_relaxed);
    const bool screenMetricsChanged = (screenW != s_lastViewportScreenW) || (screenH != s_lastViewportScreenH);

    if (currentModeId == s_lastCachedModeId && snapVer == s_lastCachedViewportSnapshotVersion && !guiOpen && !periodicRefresh &&
        !screenMetricsChanged) {
        return;
    }

    if (periodicRefresh) { s_ticksSinceRefresh = 0; }

    // Get mode data via config snapshot (thread-safe, lock-free)
    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap) return;
    const ModeConfig* mode = GetModeFromSnapshotOrFallback(*cfgSnap, currentModeId);

    int nextIndex = 1 - g_viewportModeCacheIndex.load(std::memory_order_relaxed);
    CachedModeViewport& cache = g_viewportModeCache[nextIndex];

    if (mode) {
        cache.width = mode->width;
        cache.height = mode->height;
        cache.stretchEnabled = mode->stretch.enabled;
        if (mode->stretch.enabled && EqualsIgnoreCase(mode->id, "Fullscreen")) {
            cache.stretchX = 0;
            cache.stretchY = 0;
            cache.stretchWidth = screenW;
            cache.stretchHeight = screenH;
        } else {
            cache.stretchX = mode->stretch.x;
            cache.stretchY = mode->stretch.y;
            cache.stretchWidth = mode->stretch.width;
            cache.stretchHeight = mode->stretch.height;
        }
        cache.valid = true;
    } else {
        cache.valid = false;
    }

    // Atomic swap to make new cache visible
    g_viewportModeCacheIndex.store(nextIndex, std::memory_order_release);
    s_lastCachedModeId = currentModeId;
    s_lastCachedViewportSnapshotVersion = snapVer;
    s_lastViewportScreenW = screenW;
    s_lastViewportScreenH = screenH;
}

void PollObsGraphicsHook() {
    PROFILE_SCOPE_CAT("LT OBS Hook Poll", "Logic Thread");
    auto now = std::chrono::steady_clock::now();
    auto msSinceLastCheck = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_lastGraphicsHookCheck).count();

    if (msSinceLastCheck >= GRAPHICS_HOOK_CHECK_INTERVAL_MS) {
        g_lastGraphicsHookCheck = now;
        HMODULE hookModule = GetModuleHandleA("graphics-hook64.dll");
        bool wasDetected = g_graphicsHookDetected.load();
        bool nowDetected = (hookModule != NULL);

        if (nowDetected != wasDetected) {
            g_graphicsHookDetected.store(nowDetected);
            g_graphicsHookModule.store(hookModule);
            if (nowDetected) {
                Log("[OBS] graphics-hook64.dll DETECTED - OBS overlay active");
            } else {
                Log("[OBS] graphics-hook64.dll UNLOADED - OBS overlay inactive");
            }
        }
    }
}

void CheckWorldExitReset() {
    PROFILE_SCOPE_CAT("LT World Exit Check", "Logic Thread");

    // Get current game state from lock-free buffer
    std::string currentGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];
    bool isInWorld = (currentGameState.find("inworld") != std::string::npos);

    if (s_wasInWorld && !isInWorld) {
        auto cfgSnap = GetConfigSnapshot();
        if (cfgSnap) {
            const Config& cfg = *cfgSnap;
            for (size_t i = 0; i < cfg.hotkeys.size(); ++i) {
                const auto& hotkey = cfg.hotkeys[i];
                if (!hotkey.secondaryMode.empty() && GetHotkeySecondaryMode(i) != hotkey.secondaryMode) {
                    SetHotkeySecondaryMode(i, hotkey.secondaryMode);
                    Log("[Hotkey] Reset secondary mode for hotkey to: " + hotkey.secondaryMode);
                }
            }
        }
    }
    s_wasInWorld = isInWorld;
}

void CheckWindowsMouseSpeedChange() {
    PROFILE_SCOPE_CAT("LT Mouse Speed Check", "Logic Thread");
    auto cfgSnap = GetConfigSnapshot();
    int currentWindowsMouseSpeed = cfgSnap ? cfgSnap->windowsMouseSpeed : 0;
    if (currentWindowsMouseSpeed != s_lastAppliedWindowsMouseSpeed) {
        ApplyWindowsMouseSpeed();
        s_lastAppliedWindowsMouseSpeed = currentWindowsMouseSpeed;
    }
}

void ProcessPendingModeSwitch() {
    PROFILE_SCOPE_CAT("LT Mode Switch", "Logic Thread");
    std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
    if (!g_pendingModeSwitch.pending) { return; }

    const HWND hwnd = g_minecraftHwnd.load(std::memory_order_acquire);
    if (hwnd) {
        const DWORD windowThreadId = GetWindowThreadProcessId(hwnd, nullptr);
        const DWORD currentThreadId = GetCurrentThreadId();
        if (windowThreadId != 0 && windowThreadId != currentThreadId) {
            LogCategory("gui", "[GUI] Forwarding pending mode switch to window thread: " + g_pendingModeSwitch.modeId);
            PostMessage(hwnd, WM_TOOLSCREEN_SWITCH_MODE, 0, 0);
            return;
        }
    }

    if (g_pendingModeSwitch.isPreview && !g_pendingModeSwitch.previewFromModeId.empty()) {
        Log("[GUI] Processing preview mode switch: " + g_pendingModeSwitch.previewFromModeId + " -> " + g_pendingModeSwitch.modeId);

        std::string fromModeId = g_pendingModeSwitch.previewFromModeId;
        std::string toModeId = g_pendingModeSwitch.modeId;

        SwitchToMode(fromModeId, "Preview (instant)", /*forceCut=*/true);

        SwitchToMode(toModeId, "Preview (animated)");
    } else {
        LogCategory("gui", "[GUI] Processing deferred mode switch to: " + g_pendingModeSwitch.modeId +
                               " (source: " + g_pendingModeSwitch.source + ")");

        // This avoids cross-thread mutation of g_config from the logic thread
        SwitchToMode(g_pendingModeSwitch.modeId, g_pendingModeSwitch.source,
                     /*forceCut=*/g_pendingModeSwitch.forceInstant);
    }

    g_pendingModeSwitch.pending = false;
    g_pendingModeSwitch.isPreview = false;
    g_pendingModeSwitch.forceInstant = false;
    g_pendingModeSwitch.modeId.clear();
    g_pendingModeSwitch.source.clear();
    g_pendingModeSwitch.previewFromModeId.clear();
}

void CheckGameStateReset() {
    PROFILE_SCOPE_CAT("LT Game State Reset", "Logic Thread");

    if (!IsResolutionChangeSupported(g_gameVersion)) { return; }

    // Get current game state from lock-free buffer
    std::string localGameState = g_gameStateBuffers[g_currentGameStateIndex.load(std::memory_order_acquire)];

    if (isWallTitleOrWaiting(localGameState) && !isWallTitleOrWaiting(s_previousGameStateForReset)) {
        auto cfgSnap = GetConfigSnapshot();
        if (cfgSnap) {
            const Config& cfg = *cfgSnap;
            for (size_t i = 0; i < cfg.hotkeys.size(); ++i) {
                if (GetHotkeySecondaryMode(i) != cfg.hotkeys[i].secondaryMode) { SetHotkeySecondaryMode(i, cfg.hotkeys[i].secondaryMode); }
            }

            std::string targetMode = cfg.defaultMode;
            Log("[LogicThread] Reset all hotkey secondary modes to default due to wall/title/waiting state.");
            const HWND hwnd = g_minecraftHwnd.load(std::memory_order_acquire);
            const DWORD windowThreadId = hwnd ? GetWindowThreadProcessId(hwnd, nullptr) : 0;
            if (hwnd && windowThreadId != 0 && windowThreadId != GetCurrentThreadId()) {
                std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
                if (!g_pendingModeSwitch.pending) {
                    g_pendingModeSwitch.pending = true;
                    g_pendingModeSwitch.modeId = targetMode;
                    g_pendingModeSwitch.source = "game state reset";
                    g_pendingModeSwitch.forceInstant = true;
                    LogCategory("gui", "[LogicThread] Forwarding game state reset to window thread.");
                    PostMessage(hwnd, WM_TOOLSCREEN_SWITCH_MODE, 0, 0);
                }
            } else {
                SwitchToMode(targetMode, "game state reset", /*forceCut=*/true);
            }
        }
    }

    s_previousGameStateForReset = localGameState;
}

static void CheckAutoBorderless() {
    static bool s_checked = false;
    if (s_checked) { return; }

    HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
    if (!hwnd) { return; }

    auto cfgSnap = GetConfigSnapshot();
    if (!cfgSnap || !cfgSnap->autoBorderless) {
        s_checked = true;
        return;
    }

    if (g_originalWndProc == NULL) {
        // Wait until SubclassedWndProc is active so the custom toggle message is handled on the window thread.
        return;
    }

    const UINT toggleMsg = GetToolscreenBorderlessToggleMessageId();
    if (!PostMessage(hwnd, toggleMsg, 0, 0)) {
        Log("[LogicThread] Failed to queue auto-borderless message. Error=" + std::to_string(GetLastError()));
        return;
    }

    s_checked = true;
    Log("[LogicThread] Auto-borderless queued");
}

static void LogicThreadFunc() {
    LogCategory("init", "[LogicThread] Started");

    const auto tickInterval = std::chrono::milliseconds(16);

    while (!g_logicThreadShouldStop.load()) {
        PROFILE_SCOPE_CAT("Logic Thread Tick", "Logic Thread");
        auto tickStart = std::chrono::steady_clock::now();

        if (g_isShuttingDown.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        if (!g_configLoaded.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        UpdateCachedScreenMetrics();
        UpdateCachedViewportMode();
        UpdateActiveMirrorConfigs();
        PollObsGraphicsHook();
        CheckWorldExitReset();
        CheckWindowsMouseSpeedChange();
        ProcessPendingModeSwitch();
        CheckGameStateReset();
        CheckAutoBorderless();

        auto tickEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(tickEnd - tickStart);
        if (elapsed < tickInterval) { std::this_thread::sleep_for(tickInterval - elapsed); }
    }

    Log("[LogicThread] Stopped");
}

void StartLogicThread() {
    if (g_logicThreadRunning.load()) {
        Log("[LogicThread] Already running, not starting again");
        return;
    }

    Log("[LogicThread] Starting logic thread...");
    g_logicThreadShouldStop.store(false);
    s_startupMetricsResyncPending.store(true, std::memory_order_relaxed);
    s_screenMetricsDirty.store(true, std::memory_order_relaxed);
    s_screenMetricsRecalcRequested.store(true, std::memory_order_relaxed);

    g_logicThread = std::thread(LogicThreadFunc);
    g_logicThreadRunning.store(true);

    LogCategory("init", "[LogicThread] Logic thread started");
}

void StopLogicThread() {
    if (!g_logicThreadRunning.load()) { return; }

    Log("[LogicThread] Stopping logic thread...");
    g_logicThreadShouldStop.store(true);

    if (g_logicThread.joinable()) { g_logicThread.join(); }

    g_logicThreadRunning.store(false);
    Log("[LogicThread] Logic thread stopped");
}


