#include "gui_internal.h"

#include "common/font_assets.h"
#include "common/mode_dimensions.h"
#include "common/profiler.h"
#include "common/utils.h"
#include "config/config_migration.h"
#include "config/config_toml.h"
#include "render/render.h"
#include "render/mirror_thread.h"
#include "runtime/logic_thread.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

static std::atomic<bool> s_isConfigSaving{ false };

struct ActiveProfileSaveState {
    bool tracked = false;
    std::string name;
    ProfileSectionSelection sections;
};

static Config BuildRuntimeResolvedConfig(const Config& source) {
    Config resolved = source;

    const int screenWidth = GetCachedWindowWidth();
    const int screenHeight = GetCachedWindowHeight();
    if (screenWidth > 0 && screenHeight > 0) {
        RecalculateModeDimensions(resolved, screenWidth, screenHeight);
    }

    SanitizeConfigKeyRebindsForCannotTypeTriggers(resolved);

    return resolved;
}

static ActiveProfileSaveState GetActiveProfileSaveState() {
    ActiveProfileSaveState state;
    state.name = g_profilesConfig.activeProfile;

    for (const auto& profile : g_profilesConfig.profiles) {
        if (EqualsIgnoreCase(profile.name, state.name)) {
            state.tracked = true;
            state.name = profile.name;
            state.sections = profile.sections;
            break;
        }
    }

    return state;
}

static void SyncSharedConfigFromMergedConfig(const ActiveProfileSaveState& state, const Config& mergedConfig) {
    if (!state.tracked) {
        g_sharedConfig = mergedConfig;
        g_sharedConfig.configVersion = GetConfigVersion();
        return;
    }

    Config updatedShared = mergedConfig;
    ApplyProfileFields(g_sharedConfig, updatedShared, state.sections);
    updatedShared.configVersion = GetConfigVersion();
    g_sharedConfig = std::move(updatedShared);
}

static ActiveProfileSaveState PrepareConfigPersistence(Config& sharedSnapshot, Config& profileSnapshot) {
    const ActiveProfileSaveState state = GetActiveProfileSaveState();
    const Config runtimeResolvedConfig = BuildRuntimeResolvedConfig(g_config);

    SyncSharedConfigFromMergedConfig(state, runtimeResolvedConfig);
    sharedSnapshot = g_sharedConfig;

    if (!state.tracked) {
        profileSnapshot = Config{};
        return state;
    }

    profileSnapshot = Config{};
    ApplyProfileFields(runtimeResolvedConfig, profileSnapshot, state.sections);
    profileSnapshot.configVersion = GetConfigVersion();
    return state;
}

void PublishGuiConfigSnapshot() {
    const ActiveProfileSaveState state = GetActiveProfileSaveState();
    const Config runtimeResolvedConfig = BuildRuntimeResolvedConfig(g_config);
    SyncSharedConfigFromMergedConfig(state, runtimeResolvedConfig);
    PublishConfigSnapshot(runtimeResolvedConfig);
}

bool WaitForConfigSaveIdle(int timeoutMs) {
    const auto startWait = std::chrono::steady_clock::now();
    while (s_isConfigSaving.load(std::memory_order_acquire)) {
        if (timeoutMs >= 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startWait).count() > timeoutMs) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

std::string GameTransitionTypeToString(GameTransitionType type) {
    switch (type) {
    case GameTransitionType::Cut:
        return "Cut";
    case GameTransitionType::Bounce:
        return "Bounce";
    default:
        return "Bounce";
    }
}

GameTransitionType StringToGameTransitionType(const std::string& str) {
    if (str == "Cut") return GameTransitionType::Cut;
    return GameTransitionType::Bounce;
}

std::string OverlayTransitionTypeToString(OverlayTransitionType type) {
    switch (type) {
    case OverlayTransitionType::Cut:
        return "Cut";
    default:
        return "Cut";
    }
}

OverlayTransitionType StringToOverlayTransitionType(const std::string&) { return OverlayTransitionType::Cut; }

std::string BackgroundTransitionTypeToString(BackgroundTransitionType type) {
    switch (type) {
    case BackgroundTransitionType::Cut:
        return "Cut";
    default:
        return "Cut";
    }
}

BackgroundTransitionType StringToBackgroundTransitionType(const std::string&) { return BackgroundTransitionType::Cut; }

bool RemoveInvalidHotkeyModeReferences(Config& config) {
    auto modeExists = [&](const std::string& modeId) -> bool {
        if (modeId.empty()) { return false; }
        for (const auto& mode : config.modes) {
            if (EqualsIgnoreCase(mode.id, modeId)) { return true; }
        }
        return false;
    };

    std::string fallbackMainMode;
    if (modeExists(config.defaultMode)) {
        fallbackMainMode = config.defaultMode;
    } else if (modeExists("Fullscreen")) {
        fallbackMainMode = "Fullscreen";
    } else if (!config.modes.empty()) {
        fallbackMainMode = config.modes.front().id;
    }

    bool changed = false;
    size_t mainModeResetCount = 0;
    size_t secondaryModeClearedCount = 0;
    size_t altModeRemovedCount = 0;
    for (auto& hotkey : config.hotkeys) {
        if (hotkey.mainMode.empty()) {
            if (!fallbackMainMode.empty()) {
                hotkey.mainMode = fallbackMainMode;
                changed = true;
                ++mainModeResetCount;
            }
        } else if (!modeExists(hotkey.mainMode)) {
            if (hotkey.mainMode != fallbackMainMode) {
                hotkey.mainMode = fallbackMainMode;
                changed = true;
                ++mainModeResetCount;
            }
        }

        if (!hotkey.secondaryMode.empty() && !modeExists(hotkey.secondaryMode)) {
            hotkey.secondaryMode.clear();
            changed = true;
            ++secondaryModeClearedCount;
        }

        const auto newEnd = std::remove_if(hotkey.altSecondaryModes.begin(), hotkey.altSecondaryModes.end(),
                                           [&](const AltSecondaryMode& alt) {
                                               return !alt.mode.empty() && !modeExists(alt.mode);
                                           });
        if (newEnd != hotkey.altSecondaryModes.end()) {
            altModeRemovedCount += static_cast<size_t>(std::distance(newEnd, hotkey.altSecondaryModes.end()));
            hotkey.altSecondaryModes.erase(newEnd, hotkey.altSecondaryModes.end());
            changed = true;
        }
    }

    if (changed) {
        Log("Sanitized hotkey mode references: reset " + std::to_string(mainModeResetCount) + " main mode reference(s), cleared " +
            std::to_string(secondaryModeClearedCount) + " secondary mode reference(s), removed " +
            std::to_string(altModeRemovedCount) + " alt mode reference(s).");
    }

    return changed;
}

void CopyToClipboard(HWND hwnd, const std::string& text) {
    if (!OpenClipboard(hwnd)) {
        Log("ERROR: Could not open clipboard. Error code: " + std::to_string(GetLastError()));
        return;
    }

    struct ClipboardGuard {
        ~ClipboardGuard() { CloseClipboard(); }
    } guard;

    if (!EmptyClipboard()) {
        Log("ERROR: Could not empty clipboard. Error code: " + std::to_string(GetLastError()));
        return;
    }

    std::wstring wideText = Utf8ToWide(text);
    size_t size = (wideText.length() + 1) * sizeof(WCHAR);

    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hg) {
        Log("ERROR: GlobalAlloc failed. Error code: " + std::to_string(GetLastError()));
        return;
    }

    void* globalData = GlobalLock(hg);
    if (!globalData) {
        Log("ERROR: GlobalLock failed. Error code: " + std::to_string(GetLastError()));
        GlobalFree(hg);
        return;
    }

    memcpy(globalData, wideText.c_str(), size);
    GlobalUnlock(hg);

    if (!SetClipboardData(CF_UNICODETEXT, hg)) {
        Log("ERROR: SetClipboardData failed. Error code: " + std::to_string(GetLastError()));
        GlobalFree(hg);
    }
}

void ParseColorString(const std::string& input, Color& outColor) {
    std::string s = input;
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    if (s.rfind('#', 0) == 0) s = s.substr(1);
    if (s.length() == 6 && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
        try {
            unsigned long value = std::stoul(s, nullptr, 16);
            outColor = { ((value >> 16) & 0xFF) / 255.0f, ((value >> 8) & 0xFF) / 255.0f, (value & 0xFF) / 255.0f };
            return;
        } catch (...) {
        }
    }

    std::stringstream ss(s);
    std::string item;
    float components[3];
    int i = 0;
    while (std::getline(ss, item, ',') && i < 3) {
        try {
            components[i++] = std::stof(item);
        } catch (...) {
            goto error_case;
        }
    }
    if (i == 3) {
        outColor = { components[0] / 255.0f, components[1] / 255.0f, components[2] / 255.0f };
        return;
    }

error_case:
    Log("ERROR: Invalid color format: '" + input + "'. Using black as default.");
    outColor = { 0.0f, 0.0f, 0.0f };
}

void SaveConfig() {
    PROFILE_SCOPE_CAT("Config Save", "IO Operations");

    static auto s_lastSaveTime = std::chrono::steady_clock::now();

    auto currentTime = std::chrono::steady_clock::now();
    auto timeSinceLastSave = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - s_lastSaveTime).count();

    if (!g_configIsDirty.load()) return;
    if (timeSinceLastSave < 1000) return;
    if (s_isConfigSaving.load()) return;

    if (g_toolscreenPath.empty()) {
        Log("ERROR: Cannot save config, toolscreen path is not available.");
        return;
    }

    std::wstring configPath = g_toolscreenPath + L"\\config.toml";
    try {
        Config sharedSnapshot;
        Config profileSnapshot;
        const ActiveProfileSaveState activeProfileState = PrepareConfigPersistence(sharedSnapshot, profileSnapshot);

        const std::string activeProfileName = activeProfileState.name;
        const bool activeProfileTracked = activeProfileState.tracked;

        PublishGuiConfigSnapshot();

        g_configIsDirty = false;
        s_lastSaveTime = currentTime;
        s_isConfigSaving = true;

        std::thread([configPath, sharedSnapshot = std::move(sharedSnapshot), activeProfileName, activeProfileTracked,
                     profileSnapshot = std::move(profileSnapshot)]() {
            _set_se_translator(SEHTranslator);
            try {
                try {
                    if (!SaveConfigToTomlFile(sharedSnapshot, configPath)) {
                        Log("ERROR: Failed to write config file.");
                    }
                } catch (const std::exception& e) {
                    Log("ERROR: Failed to write config file: " + std::string(e.what()));
                }
                if (activeProfileTracked && !SaveProfileSnapshotIfTracked(activeProfileName, profileSnapshot)) {
                    Log("INFO: Skipped async profile save for removed or renamed profile '" + activeProfileName + "'.");
                }
            } catch (const SE_Exception& e) {
                LogException("ConfigSaveThread (SEH)", e.getCode(), e.getInfo());
            } catch (const std::exception& e) {
                LogException("ConfigSaveThread", e);
            } catch (...) {
                Log("EXCEPTION in ConfigSaveThread: Unknown exception");
            }
            s_isConfigSaving = false;
        }).detach();
    } catch (const std::exception& e) {
        Log("ERROR: Failed to prepare config for save: " + std::string(e.what()));
    } catch (...) {
        Log("ERROR: Unknown exception in SaveConfig");
    }
}

void SaveConfigImmediate() {
    PROFILE_SCOPE_CAT("Config Save (Immediate)", "IO Operations");

    if (s_isConfigSaving.load()) {
        Log("SaveConfigImmediate: Waiting for background save to complete...");
        if (!WaitForConfigSaveIdle(3000)) {
            Log("SaveConfigImmediate: Timed out waiting for background save. Proceeding anyway.");
        }
    }

    if (!g_configIsDirty) return;

    if (g_toolscreenPath.empty()) {
        Log("ERROR: Cannot save config, toolscreen path is not available.");
        return;
    }

    std::wstring configPath = g_toolscreenPath + L"\\config.toml";
    try {
        Log("SaveConfigImmediate: Starting config copy...");
        Config sharedSnapshot;
        Config profileSnapshot;
        const ActiveProfileSaveState activeProfileState = PrepareConfigPersistence(sharedSnapshot, profileSnapshot);

        PublishGuiConfigSnapshot();

        if (!SaveConfigToTomlFile(sharedSnapshot, configPath)) {
            Log("ERROR: Failed to write config file.");
            return;
        }

        if (activeProfileState.tracked && !SaveProfileSnapshotIfTracked(activeProfileState.name, profileSnapshot)) {
            Log("INFO: Skipped immediate profile save for removed or renamed profile '" + activeProfileState.name + "'.");
        }

        Log("Configuration saved to file (immediate).");
        g_configIsDirty = false;
    } catch (const std::exception& e) {
        Log("ERROR: Failed to write config file: " + std::string(e.what()));
    } catch (...) {
        Log("ERROR: Unknown exception in SaveConfigImmediate");
    }
}

std::vector<ModeConfig> GetDefaultModes() { return GetDefaultModesFromEmbedded(); }
std::vector<MirrorConfig> GetDefaultMirrors() { return GetDefaultMirrorsFromEmbedded(); }
std::vector<MirrorGroupConfig> GetDefaultMirrorGroups() { return GetDefaultMirrorGroupsFromEmbedded(); }
std::vector<ImageConfig> GetDefaultImages() { return GetDefaultImagesFromEmbedded(); }

std::vector<WindowOverlayConfig> GetDefaultWindowOverlays() {
    return std::vector<WindowOverlayConfig>();
}

std::vector<BrowserOverlayConfig> GetDefaultBrowserOverlays() {
    return std::vector<BrowserOverlayConfig>();
}

std::vector<HotkeyConfig> GetDefaultHotkeys() { return GetDefaultHotkeysFromEmbedded(); }
CursorsConfig GetDefaultCursors() { return GetDefaultCursorsFromEmbedded(); }

void WriteDefaultConfig(const std::wstring& path) {
    int screenWidth = GetCachedWindowWidth();
    int screenHeight = GetCachedWindowHeight();

    Config defaultConfig;
    if (LoadEmbeddedDefaultConfig(defaultConfig)) {
        for (auto& mode : defaultConfig.modes) {
            if (mode.id == "Fullscreen") {
                mode.width = screenWidth;
                mode.height = screenHeight;
                if (mode.stretch.enabled) {
                    mode.stretch.width = screenWidth;
                    mode.stretch.height = screenHeight;
                }
            }

            mode.manualWidth = mode.width;
            mode.manualHeight = mode.height;
        }

        int eyezoomWindowWidth = defaultConfig.eyezoom.windowWidth;
        if (eyezoomWindowWidth < 1) eyezoomWindowWidth = ConfigDefaults::EYEZOOM_WINDOW_WIDTH;
        int eyezoomTargetFinalX = (screenWidth - eyezoomWindowWidth) / 2;
        if (eyezoomTargetFinalX < 1) eyezoomTargetFinalX = 1;
        int horizontalMargin = ((screenWidth / 2) - (eyezoomWindowWidth / 2)) / 10;
        int verticalMargin = (screenHeight / 2) / 4;
        int defaultZoomAreaWidth = eyezoomTargetFinalX - (2 * horizontalMargin);
        int defaultZoomAreaHeight = screenHeight - (2 * verticalMargin);
        if (defaultZoomAreaWidth < 1) defaultZoomAreaWidth = 1;
        if (defaultZoomAreaHeight < 1) defaultZoomAreaHeight = 1;

        defaultConfig.eyezoom.zoomAreaWidth = defaultZoomAreaWidth;
        defaultConfig.eyezoom.zoomAreaHeight = defaultZoomAreaHeight;
        defaultConfig.eyezoom.positionX = horizontalMargin;
        defaultConfig.eyezoom.positionY = verticalMargin;

        for (auto& image : defaultConfig.images) {
            if (image.name == "Ninjabrain Bot" && image.path.empty()) {
                WCHAR tempPath[MAX_PATH];
                if (GetTempPathW(MAX_PATH, tempPath) > 0) {
                    std::wstring nbImagePath = std::wstring(tempPath) + L"nb-overlay.png";
                    image.path = WideToUtf8(nbImagePath);
                }
            }
        }

        HDC hdc = GetDC(NULL);
        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(NULL, hdc);
        int systemCursorSize = GetSystemMetricsForDpi(SM_CYCURSOR, dpi);
        systemCursorSize = std::clamp(systemCursorSize, ConfigDefaults::CURSOR_MIN_SIZE, ConfigDefaults::CURSOR_MAX_SIZE);
        defaultConfig.cursors.title.cursorSize = systemCursorSize;
        defaultConfig.cursors.wall.cursorSize = systemCursorSize;
        defaultConfig.cursors.ingame.cursorSize = systemCursorSize;

        try {
            if (!SaveConfigToTomlFile(defaultConfig, path)) {
                Log("ERROR: Failed to write default config file.");
                return;
            }
            Log("Wrote default config.toml from embedded defaults, customized for your monitor (" + std::to_string(screenWidth) + "x" +
                std::to_string(screenHeight) + ").");
        } catch (const std::exception& e) {
            Log("ERROR: Failed to write default config file: " + std::string(e.what()));
        }
    } else {
        Log("WARNING: Could not load embedded default config, creating minimal fallback config");
        defaultConfig.configVersion = GetConfigVersion();
        defaultConfig.defaultMode = "Fullscreen";
        defaultConfig.guiHotkey = ConfigDefaults::GetDefaultGuiHotkey();

        ModeConfig fullscreenMode;
        fullscreenMode.id = "Fullscreen";
        fullscreenMode.width = screenWidth;
        fullscreenMode.height = screenHeight;
        fullscreenMode.manualWidth = fullscreenMode.width;
        fullscreenMode.manualHeight = fullscreenMode.height;
        fullscreenMode.stretch.enabled = true;
        fullscreenMode.stretch.width = screenWidth;
        fullscreenMode.stretch.height = screenHeight;
        defaultConfig.modes.push_back(fullscreenMode);

        try {
            if (!SaveConfigToTomlFile(defaultConfig, path)) {
                Log("ERROR: Failed to write fallback config file.");
                return;
            }
            Log("Wrote fallback config.toml for your monitor (" + std::to_string(screenWidth) + "x" + std::to_string(screenHeight) + ").");
        } catch (const std::exception& e) {
            Log("ERROR: Failed to write fallback config file: " + std::string(e.what()));
        }
    }
}

void LoadConfig() {
    PROFILE_SCOPE_CAT("Config Load", "IO Operations");
    if (g_toolscreenPath.empty()) {
        Log("Cannot load config, toolscreen path is not available.");
        return;
    }

    std::wstring configPath = g_toolscreenPath + L"\\config.toml";

    if (!std::filesystem::exists(configPath)) {
        Log("config.toml not found. Writing a default config file.");
        WriteDefaultConfig(configPath);
        if (!std::filesystem::exists(configPath)) {
            std::string errorMessage = "FATAL: Could not create or read default config. Aborting load.";
            Log(errorMessage);
            g_configLoadFailed = true;
            {
                std::lock_guard<std::mutex> lock(g_configErrorMutex);
                g_configLoadError = errorMessage;
            }
            return;
        }
    }

    BackupConfigFile();
    ExtractBundledFontAssets(std::filesystem::path(g_toolscreenPath), &LoadConfig);
    LoadSystemFontAssets();

    try {
        g_config = Config();
        g_sharedConfig = Config();
        {
            std::lock_guard<std::mutex> lock(g_hotkeyTimestampsMutex);
            g_hotkeyTimestamps.clear();
        }

        std::ifstream in(std::filesystem::path(configPath), std::ios::binary);
        if (!in.is_open()) {
            throw std::runtime_error("Failed to open config.toml for reading.");
        }

        toml::table tbl;
#if TOML_EXCEPTIONS
        tbl = toml::parse(in, configPath);
#else
        toml::parse_result result = toml::parse(in, configPath);
        if (!result) {
            const auto& err = result.error();
            throw std::runtime_error(std::string(err.description()));
        }
        tbl = std::move(result).table();
#endif
        ConfigFromToml(tbl, g_config);
        g_sharedConfig = g_config;

        MigrateToProfiles();
        EnsureProfilesConfigReady();
        if (!LoadProfile(g_profilesConfig.activeProfile)) {
            Log("WARNING: Failed to load active profile '" + g_profilesConfig.activeProfile + "', using base config");
        }

        auto normalizeConfigFontPaths = [](Config& config, const std::wstring& toolscreenPath) {
            if (config.fontPath.empty()) {
                config.fontPath = ConfigDefaults::CONFIG_DEFAULT_GUI_FONT_PATH;
            } else {
                NormalizeBundledFontPathInPlace(config.fontPath, toolscreenPath);
            }

            NormalizeBundledFontPathInPlace(config.eyezoom.textFontPath, toolscreenPath);

            if (config.ninjabrainOverlay.customFontPath.empty()) {
                config.ninjabrainOverlay.customFontPath = ConfigDefaults::CONFIG_FONT_PATH;
            } else {
                NormalizeBundledFontPathInPlace(config.ninjabrainOverlay.customFontPath, toolscreenPath);
            }
        };

        normalizeConfigFontPaths(g_sharedConfig, g_toolscreenPath);
        normalizeConfigFontPaths(g_config, g_toolscreenPath);
        Log("Loaded config from TOML file.");

        int screenWidth = GetCachedWindowWidth();
        int screenHeight = GetCachedWindowHeight();
        if (screenWidth < 1) screenWidth = 1;
        if (screenHeight < 1) screenHeight = 1;

        auto modeExists = [&](const std::string& id) -> bool {
            for (const auto& mode : g_config.modes) {
                if (EqualsIgnoreCase(mode.id, id)) return true;
            }
            return false;
        };

        if (!modeExists("Fullscreen")) {
            ModeConfig fullscreenMode;
            fullscreenMode.id = "Fullscreen";
            fullscreenMode.width = screenWidth;
            fullscreenMode.height = screenHeight;
            fullscreenMode.manualWidth = fullscreenMode.width;
            fullscreenMode.manualHeight = fullscreenMode.height;
            fullscreenMode.stretch.enabled = true;
            fullscreenMode.stretch.x = 0;
            fullscreenMode.stretch.y = 0;
            fullscreenMode.stretch.width = screenWidth;
            fullscreenMode.stretch.height = screenHeight;
            AddModeSource(fullscreenMode, ModeSourceType::Mirror, "Mapless");
            g_config.modes.insert(g_config.modes.begin(), fullscreenMode);
            Log("Created missing Fullscreen mode");
        }

        if (!modeExists("EyeZoom")) {
            ModeConfig eyezoomMode;
            eyezoomMode.id = "EyeZoom";
            eyezoomMode.width = 384;
            eyezoomMode.height = 16384;
            eyezoomMode.manualWidth = eyezoomMode.width;
            eyezoomMode.manualHeight = eyezoomMode.height;
            g_config.modes.push_back(eyezoomMode);
            Log("Created missing EyeZoom mode");
        }

        {
            ModeConfig* eyezoomModePtr = nullptr;
            for (auto& m : g_config.modes) {
                if (EqualsIgnoreCase(m.id, "EyeZoom")) {
                    eyezoomModePtr = &m;
                    break;
                }
            }

            if (!modeExists("Preemptive")) {
                ModeConfig preemptiveMode;
                if (eyezoomModePtr) { preemptiveMode = *eyezoomModePtr; }
                preemptiveMode.id = "Preemptive";
                preemptiveMode.width = eyezoomModePtr ? eyezoomModePtr->width : 384;
                preemptiveMode.height = eyezoomModePtr ? eyezoomModePtr->height : 16384;
                preemptiveMode.manualWidth = eyezoomModePtr ? eyezoomModePtr->manualWidth : preemptiveMode.width;
                preemptiveMode.manualHeight = eyezoomModePtr ? eyezoomModePtr->manualHeight : preemptiveMode.height;
                preemptiveMode.useRelativeSize = false;
                preemptiveMode.relativeWidth = -1.0f;
                preemptiveMode.relativeHeight = -1.0f;
                g_config.modes.push_back(preemptiveMode);
                Log("Created missing Preemptive mode");
            } else {
                ModeConfig* preemptiveModePtr = nullptr;
                for (auto& m : g_config.modes) {
                    if (EqualsIgnoreCase(m.id, "Preemptive")) {
                        preemptiveModePtr = &m;
                        break;
                    }
                }

                if (preemptiveModePtr) {
                    bool changed = false;
                    if (preemptiveModePtr->relativeWidth >= 0.0f || preemptiveModePtr->relativeHeight >= 0.0f ||
                        preemptiveModePtr->useRelativeSize) {
                        preemptiveModePtr->relativeWidth = -1.0f;
                        preemptiveModePtr->relativeHeight = -1.0f;
                        preemptiveModePtr->useRelativeSize = false;
                        changed = true;
                    }

                    if (eyezoomModePtr) {
                        if (preemptiveModePtr->width != eyezoomModePtr->width) {
                            preemptiveModePtr->width = eyezoomModePtr->width;
                            preemptiveModePtr->manualWidth =
                                (eyezoomModePtr->manualWidth > 0) ? eyezoomModePtr->manualWidth : eyezoomModePtr->width;
                            changed = true;
                        }
                        if (preemptiveModePtr->height != eyezoomModePtr->height) {
                            preemptiveModePtr->height = eyezoomModePtr->height;
                            preemptiveModePtr->manualHeight =
                                (eyezoomModePtr->manualHeight > 0) ? eyezoomModePtr->manualHeight : eyezoomModePtr->height;
                            changed = true;
                        }
                    }

                    if (changed) { g_configIsDirty = true; }
                }
            }
        }

        if (!modeExists("Thin")) {
            ModeConfig thinMode;
            thinMode.id = "Thin";
            thinMode.width = 330;
            thinMode.height = screenHeight;
            thinMode.manualWidth = thinMode.width;
            thinMode.manualHeight = thinMode.height;
            thinMode.background.selectedMode = "color";
            thinMode.background.color = { 45 / 255.0f, 0 / 255.0f, 80 / 255.0f };
            AddModeSource(thinMode, ModeSourceType::Mirror, "Mapless");
            g_config.modes.push_back(thinMode);
            Log("Created missing Thin mode");
        }

        if (!modeExists("Wide")) {
            ModeConfig wideMode;
            wideMode.id = "Wide";
            wideMode.width = screenWidth;
            wideMode.height = 400;
            wideMode.manualWidth = wideMode.width;
            wideMode.manualHeight = wideMode.height;
            wideMode.background.selectedMode = "color";
            wideMode.background.color = { 0.0f, 0.0f, 0.0f };
            AddModeSource(wideMode, ModeSourceType::Mirror, "Mapless");
            g_config.modes.push_back(wideMode);
            Log("Created missing Wide mode");
        }

        int clientWidth = 0;
        int clientHeight = 0;
        bool hasClientMetrics = false;
        {
            RECT clientRect{};
            HWND hwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
            if (GetWindowClientRectInScreen(hwnd, clientRect)) {
                clientWidth = clientRect.right - clientRect.left;
                clientHeight = clientRect.bottom - clientRect.top;
                hasClientMetrics = clientWidth > 0 && clientHeight > 0;
            }
        }

        for (auto& mode : g_config.modes) {
            bool widthIsRelative = mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f;
            bool heightIsRelative = mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f;

            if (widthIsRelative && hasClientMetrics) {
                mode.width = static_cast<int>(std::lround(mode.relativeWidth * static_cast<float>(clientWidth)));
                if (mode.width < 1) mode.width = 1;
            }
            if (heightIsRelative && hasClientMetrics) {
                mode.height = static_cast<int>(std::lround(mode.relativeHeight * static_cast<float>(clientHeight)));
                if (mode.height < 1) mode.height = 1;
            }

            if (EqualsIgnoreCase(mode.id, "Thin") && mode.width < 330) {
                mode.width = 330;
                g_configIsDirty = true;
            }

            if (EqualsIgnoreCase(mode.id, "Fullscreen")) {
                const int targetW = hasClientMetrics ? clientWidth : screenWidth;
                const int targetH = hasClientMetrics ? clientHeight : screenHeight;

                if (targetW > 0 && mode.width < 1) {
                    mode.width = targetW;
                    g_configIsDirty = true;
                }
                if (targetH > 0 && mode.height < 1) {
                    mode.height = targetH;
                    g_configIsDirty = true;
                }

                if (mode.manualWidth < 1 && mode.width > 0) {
                    mode.manualWidth = mode.width;
                    g_configIsDirty = true;
                }
                if (mode.manualHeight < 1 && mode.height > 0) {
                    mode.manualHeight = mode.height;
                    g_configIsDirty = true;
                }

                if (!mode.stretch.enabled || mode.stretch.x != 0 || mode.stretch.y != 0 || mode.stretch.width != targetW ||
                    mode.stretch.height != targetH) {
                    mode.stretch.enabled = true;
                    mode.stretch.x = 0;
                    mode.stretch.y = 0;
                    mode.stretch.width = targetW;
                    mode.stretch.height = targetH;
                    g_configIsDirty = true;
                }
            }
        }

        if (RemoveInvalidHotkeyModeReferences(g_config)) { g_configIsDirty = true; }

        ResetAllHotkeySecondaryModes();

        {
            std::lock_guard<std::mutex> lock(g_modeIdMutex);
            if (g_currentModeId.empty()) {
                g_currentModeId = g_config.defaultMode;
                int nextIndex = 1 - g_currentModeIdIndex.load(std::memory_order_relaxed);
                g_modeIdBuffers[nextIndex] = g_config.defaultMode;
                g_currentModeIdIndex.store(nextIndex, std::memory_order_release);
            }
        }

        Log("Config loaded: " + std::to_string(g_config.modes.size()) + " modes, " + std::to_string(g_config.mirrors.size()) +
            " mirrors, " + std::to_string(g_config.images.size()) + " images, " + std::to_string(g_config.windowOverlays.size()) +
            " window overlays, " + std::to_string(g_config.hotkeys.size()) + " hotkeys.");

        int loadedConfigVersion = g_config.configVersion;
        int currentConfigVersion = GetConfigVersion();

        if (MigrateConfigToCurrentVersion(g_config)) {
            g_configIsDirty = true;
            Log("Config upgraded from v" + std::to_string(loadedConfigVersion) + " to v" + std::to_string(currentConfigVersion));
        } else if (loadedConfigVersion > currentConfigVersion) {
            Log("WARNING: Config version is newer than tool version (config: v" + std::to_string(loadedConfigVersion) + ", tool: v" +
                std::to_string(currentConfigVersion) + ")");
        } else {
            Log("Config version: v" + std::to_string(loadedConfigVersion) + " (current)");
        }

        std::string initialMode;
        {
            std::lock_guard<std::mutex> lock(g_modeIdMutex);
            initialMode = g_currentModeId;
        }
        WriteCurrentModeToFile(initialMode);
        g_configIsDirty = false;
        g_configLoadFailed = false;
        {
            std::lock_guard<std::mutex> lock(g_configErrorMutex);
            g_configLoadError.clear();
        }

        {
            std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
            RebuildHotkeyMainKeys_Internal();
        }

        InvalidateConfigLookupCaches();
        SetOverlayTextFontSize(g_config.eyezoom.textFontSize);

        {
            RECT startupClientRect{};
            HWND startupHwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
            const bool hasValidStartupClient = GetWindowClientRectInScreen(startupHwnd, startupClientRect);
            if (hasValidStartupClient) {
                RecalculateModeDimensions();
            } else {
                Log("Deferring mode dimension recalculation until game client size is valid.");
            }
        }
        RequestScreenMetricsRecalculation();

        PublishGuiConfigSnapshot();
        ApplyConfineCursorToGameWindow();
        SetGlobalMirrorGammaMode(g_config.mirrorGammaMode);
        DiscardUnusedUserImageCaches();

        extern std::atomic<bool> g_configLoaded;
        g_configLoaded = true;
        Log("Config loaded successfully and marked as ready.");
    } catch (const std::exception& e) {
        std::string errorMessage = "Error parsing config.toml: " + std::string(e.what()) +
                                   "\n\nPlease fix the error in the config file or delete it to generate a new one.";
        Log(errorMessage);
        g_configLoadFailed = true;
        {
            std::lock_guard<std::mutex> lock(g_configErrorMutex);
            g_configLoadError = errorMessage;
        }
    }
}
