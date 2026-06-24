#pragma once

#include "gui.h"

#include <atomic>
#include <cfloat>
#include <climits>
#include <shared_mutex>
#include <string>
#include <vector>

struct SupporterRoleEntry {
    std::string name;
    Color color = { 1.0f, 1.0f, 1.0f, 1.0f };
    std::string imageUrl;
    int tierIconWidth = 0;
    int tierIconHeight = 0;
    std::vector<unsigned char> tierIconPixels;
    std::vector<std::string> members;
};

struct ImagePickerResult {
    bool completed = false;
    bool success = false;
    std::string path;
    std::string error;
};

struct ExclusionBindState {
    int hotkey_idx = -1;
    int exclusion_idx = -1;
};

struct AltBindState {
    int hotkey_idx = -1;
    int alt_idx = -1;
};

extern std::shared_mutex g_supportersMutex;
extern std::vector<SupporterRoleEntry> g_supporterRoles;
extern std::atomic<bool> g_supportersLoaded;
extern std::atomic<bool> g_supportersFetchEverFailed;
extern std::atomic<bool> g_supporterTierTexturesDirty;
extern int s_mainHotkeyToBind;
extern int s_sensHotkeyToBind;
extern int s_keystrokeKeyToBind;
extern ExclusionBindState s_exclusionToBind;
extern AltBindState s_altHotkeyToBind;

float ComputeGuiScaleFactorFromCachedWindowSize();
void LoadEmbeddedResourceTexture(GLuint& tex, int resourceId, int filterMode = 0x2601);
bool EnsureSupporterTierTexture(const SupporterRoleEntry& role, GLuint& outTextureId, int& outWidth, int& outHeight);
void ClearSupporterTierTextureCache();

std::string ValidateImageFile(const std::string& path, const std::wstring& toolscreenPath, int maxDimension = 16384);
ImagePickerResult OpenImagePickerAndValidate(HWND ownerHwnd, const std::wstring& initialDir, const std::wstring& toolscreenPath);
void ClearExpiredImageErrors();
void SetImageError(const std::string& key, const std::string& error);
std::string GetImageError(const std::string& key);
void ClearImageError(const std::string& key);
void StartAsyncImagePicker(const std::string& pickerId, const std::wstring& initialDir);
bool CheckAsyncImagePicker(const std::string& pickerId, std::string& outPath, std::string& outError);

void HelpMarker(const char* desc);
void SliderCtrlClickTip();
void RawInputSensitivityNote();
bool IsHotkeyBindingActive_UiState();
bool Spinner(const char* id_label, int* v, int step = 1, int min_val = INT_MIN, int max_val = INT_MAX, float inputWidth = 80.0f,
             float margin = 0.0f);
bool SpinnerDeferredTextInput(const char* id_label, int* v, int step = 1, int min_val = INT_MIN, int max_val = INT_MAX,
                              float inputWidth = 80.0f, float margin = 0.0f);
bool SpinnerFloat(const char* id_label, float* v, float step = 0.1f, float min_val = 0.0f, float max_val = FLT_MAX,
                  const char* format = "%.1f");
std::string GetKeyRepeatSliderFormat(int value, int autoValueMs, bool useSystemDefaultLabel = false);
std::string GetKeyRepeatDelaySliderFormat(int value, int autoValueMs, bool useSystemDefaultLabel = false);
void RenderTransitionSettingsHorizontalNoBackground(ModeConfig& mode, const std::string& idSuffix);
void RenderTransitionSettingsHorizontal(ModeConfig& mode, const std::string& idSuffix);
bool HasDuplicateModeName(const std::string& name, size_t currentIndex);
bool HasDuplicateMirrorName(const std::string& name, size_t currentIndex);
bool HasDuplicateImageName(const std::string& name, size_t currentIndex);
bool HasDuplicateWindowOverlayName(const std::string& name, size_t currentIndex);
bool HasDuplicateBrowserOverlayName(const std::string& name, size_t currentIndex);
bool HasDuplicateEyeZoomOverlayName(const std::string& name, size_t currentIndex);
EyeZoomConfig GetDefaultEyeZoomConfig();

namespace ImGui {
bool SliderFloatDoubleClickInput(const char* label, float* v, float v_min, float v_max, const char* format = "%.3f",
                                 ImGuiSliderFlags flags = 0);
bool SliderIntDoubleClickInput(const char* label, int* v, int v_min, int v_max, const char* format = "%d", ImGuiSliderFlags flags = 0);
}

std::vector<ModeConfig> GetDefaultModes();
std::vector<MirrorConfig> GetDefaultMirrors();
std::vector<MirrorGroupConfig> GetDefaultMirrorGroups();
std::vector<ImageConfig> GetDefaultImages();
std::vector<WindowOverlayConfig> GetDefaultWindowOverlays();
std::vector<BrowserOverlayConfig> GetDefaultBrowserOverlays();
std::vector<HotkeyConfig> GetDefaultHotkeys();
CursorsConfig GetDefaultCursors();
bool HasDuplicateMirrorGroupName(const std::string& name, size_t currentIndex);