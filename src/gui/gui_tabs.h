#pragma once

// =============================================================================
// GUI TABS - Shared header for tab implementations
// =============================================================================
// This header provides common includes and declarations for all tab files.

#include "fake_cursor.h"
#include "gui.h"
#include <imgui_impl_opengl3.h>
#include <imgui_impl_win32.h>
#include <imgui_stdlib.h>
#include "profiler.h"
#include "render.h"
#include "stb_image.h"
#include "utils.h"
#include "window_overlay.h"
#include <GL/glew.h>
#include <Shlwapi.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <shared_mutex>
#include <string>
#include <thread>

// =============================================================================
// SHARED UI HELPER FUNCTIONS
// =============================================================================

// Helper to display a little (?) mark which shows a tooltip when hovered
void HelpMarker(const char* desc);

// Spinner buttons with repeat on hold for integer values
bool Spinner(const char* id_label, int* v, int step = 1, int min_val = INT_MIN, int max_val = INT_MAX, float inputWidth = 80.0f,
             float margin = 0.0f);

// Spinner buttons with repeat on hold for float values
bool SpinnerFloat(const char* id_label, float* v, float step = 0.1f, float min_val = 0.0f, float max_val = FLT_MAX,
                  const char* format = "%.1f");

// =============================================================================
// SHARED LOOKUP TABLES
// =============================================================================

// Relative position options for mirrors (includes screen-relative options)
inline const std::vector<std::pair<const char*, const char*>>& GetRelativeToOptions() {
    static const std::vector<std::pair<const char*, const char*>> options = { { "topLeftViewport", "Top Left (Viewport)" },
                                                                              { "topRightViewport", "Top Right (Viewport)" },
                                                                              { "bottomLeftViewport", "Bottom Left (Viewport)" },
                                                                              { "bottomRightViewport", "Bottom Right (Viewport)" },
                                                                              { "centerViewport", "Center (Viewport)" },
                                                                              { "pieLeft", "Pie-Chart Left" },
                                                                              { "pieRight", "Pie-Chart Right" },
                                                                              { "topLeftScreen", "Top Left (Screen)" },
                                                                              { "topRightScreen", "Top Right (Screen)" },
                                                                              { "bottomLeftScreen", "Bottom Left (Screen)" },
                                                                              { "bottomRightScreen", "Bottom Right (Screen)" },
                                                                              { "centerScreen", "Center (Screen)" } };
    return options;
}

// Relative position options for images (simpler set without screen options)
inline const std::vector<std::pair<const char*, const char*>>& GetImageRelativeToOptions() {
    static const std::vector<std::pair<const char*, const char*>> options = { { "topLeft", "Top Left" },
                                                                              { "topRight", "Top Right" },
                                                                              { "bottomLeft", "Bottom Left" },
                                                                              { "bottomRight", "Bottom Right" },
                                                                              { "center", "Center" } };
    return options;
}

// Get friendly display name from key using options table
inline const char* GetFriendlyName(const std::string& key, const std::vector<std::pair<const char*, const char*>>& options) {
    for (const auto& option : options) {
        if (key == option.first) return option.second;
    }
    return "Unknown";
}

// Valid game states for hotkey conditions
inline const std::vector<const char*>& GetValidGameStates() {
    static const std::vector<const char*> states = { "wall",  "inworld,cursor_free", "inworld,cursor_grabbed",
                                                     "title", "waiting",             "generating" };
    return states;
}

// GUI display states (subset with "waiting" and "generating" combined)
inline const std::vector<const char*>& GetGuiGameStates() {
    static const std::vector<const char*> states = { "wall",  "inworld,cursor_free", "inworld,cursor_grabbed",
                                                     "title", "generating" };
    return states;
}

// User-friendly names for game states
inline const std::vector<std::pair<const char*, const char*>>& GetGameStateDisplayNames() {
    static const std::vector<std::pair<const char*, const char*>> names = { { "wall", "Wall Screen" },
                                                                            { "inworld,cursor_free", "In World (Cursor Free)" },
                                                                            { "inworld,cursor_grabbed", "In World (Cursor Grabbed)" },
                                                                            { "title", "Title Screen" },
                                                                            { "waiting", "Waiting Screen" },
                                                                            { "generating", "World Generation" } };
    return names;
}

inline const char* GetGameStateFriendlyName(const std::string& gameState) {
    for (const auto& pair : GetGameStateDisplayNames()) {
        if (gameState == pair.first) return pair.second;
    }
    return gameState.c_str();
}

// =============================================================================
// IMAGE PICKER HELPERS
// =============================================================================

// State for async file picker results
struct ImagePickerResult {
    bool completed = false;
    bool success = false;
    std::string path;
    std::string error;
};

// Image picker helper functions
std::string ValidateImageFile(const std::string& path, const std::wstring& toolscreenPath);
ImagePickerResult OpenImagePickerAndValidate(HWND ownerHwnd, const std::wstring& initialDir, const std::wstring& toolscreenPath);
void ClearExpiredImageErrors();
void SetImageError(const std::string& key, const std::string& error);
std::string GetImageError(const std::string& key);
void ClearImageError(const std::string& key);

// Start an async file picker for image selection
void StartAsyncImagePicker(const std::string& pickerId, const std::wstring& initialDir);
bool CheckAsyncImagePicker(const std::string& pickerId, std::string& outPath, std::string& outError);

// =============================================================================
// DUPLICATE NAME CHECKING HELPERS
// =============================================================================

bool HasDuplicateModeName(const std::string& name, size_t currentIndex);
bool HasDuplicateMirrorName(const std::string& name, size_t currentIndex);
bool HasDuplicateImageName(const std::string& name, size_t currentIndex);
bool HasDuplicateWindowOverlayName(const std::string& name, size_t currentIndex);

// =============================================================================
// TRANSITION SETTINGS UI
// =============================================================================

void RenderTransitionSettingsHorizontalNoBackground(ModeConfig& mode, const std::string& idSuffix);
void RenderTransitionSettingsHorizontal(ModeConfig& mode, const std::string& idSuffix);

// =============================================================================
// DEFAULT CONFIGURATIONS
// =============================================================================

std::vector<ModeConfig> GetDefaultModes();
std::vector<MirrorConfig> GetDefaultMirrors();
std::vector<ImageConfig> GetDefaultImages();
std::vector<WindowOverlayConfig> GetDefaultWindowOverlays();
std::vector<HotkeyConfig> GetDefaultHotkeys();
CursorsConfig GetDefaultCursors();
EyeZoomConfig GetDefaultEyeZoomConfig();

// =============================================================================
// HOTKEY BINDING STATE (shared across hotkeys and rebinds tabs)
// =============================================================================

// Binding state structures
struct ExclusionBindState {
    int hotkey_idx = -1;
    int exclusion_idx = -1;
};

struct AltBindState {
    int hotkey_idx = -1;
    int alt_idx = -1;
};

// Current binding state - defined in gui.cpp
extern int s_mainHotkeyToBind;
extern int s_sensHotkeyToBind;
extern ExclusionBindState s_exclusionToBind;
extern AltBindState s_altHotkeyToBind;

// =============================================================================
// TAB RENDER FUNCTIONS
// =============================================================================

void RenderModesTab();
void RenderMirrorsTab();
void RenderImagesTab();
void RenderWindowOverlaysTab();
void RenderHotkeysTab();
void RenderMouseTab();
void RenderSettingsTab();
void RenderRebindsTab();
