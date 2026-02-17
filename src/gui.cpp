#include "gui.h"
#include "config_toml.h"
#include "expression_parser.h"
#include "fake_cursor.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_win32.h"
#include "imgui_stdlib.h"
#include "logic_thread.h"
#include "mirror_thread.h"
#include "profiler.h"
#include "render.h"
#include "render_thread.h"
#include "resource.h"
#include "stb_image.h"
#include "utils.h"
#include "virtual_camera.h"
#include "window_overlay.h"

#include <GL/glew.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <commdlg.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <windowsx.h>

#pragma comment(lib, "Shlwapi.lib")

// Forward declarations
bool Spinner(const char* id_label, int* v, int step = 1, int min_val = INT_MIN, int max_val = INT_MAX, float inputWidth = 80.0f,
             float margin = 0.0f);
void ApplyKeyRepeatSettings();

extern std::atomic<bool> g_gameWindowActive;

// Spinner button hold configuration
static constexpr float spinnerHoldDelay = 0.2f;     // Delay before repeat starts (in seconds)
static constexpr float spinnerHoldInterval = 0.01f; // Interval between repeats (in seconds)

// Track background save status
static std::atomic<bool> s_isConfigSaving{ false };

// IMAGE VALIDATION AND FILE PICKER HELPERS
// State for async file picker results
struct ImagePickerResult {
    bool completed = false;
    bool success = false;
    std::string path;
    std::string error;
};

// Global state for file picker operations (keyed by unique identifier)
static std::atomic<bool> g_wasCursorVisible{ true };
static std::mutex g_imagePickerMutex;
static std::map<std::string, ImagePickerResult> g_imagePickerResults;
static std::map<std::string, std::future<ImagePickerResult>> g_imagePickerFutures;
static std::map<std::string, std::string> g_imageErrorMessages;                        // Persistent error messages for display
static std::map<std::string, std::chrono::steady_clock::time_point> g_imageErrorTimes; // Error message expiry

// Last key/mouse-down input event observed by WndProc, used for precise key binding capture.
static std::atomic<uint64_t> g_bindingInputEventSequence{ 0 };
static std::atomic<DWORD> g_bindingInputEventVk{ 0 };
static std::atomic<LPARAM> g_bindingInputEventLParam{ 0 };
static std::atomic<bool> g_bindingInputEventIsMouse{ false };

void RegisterBindingInputEvent(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    DWORD vk = 0;
    bool isMouseButton = false;

    auto resolveVkFromKeyboardMessage = [](WPARAM keyWParam, LPARAM keyLParam) -> DWORD {
        UINT scanCodeWithFlags = static_cast<UINT>((keyLParam >> 16) & 0xFF);
        if ((keyLParam & (1LL << 24)) != 0) { scanCodeWithFlags |= 0xE000; }

        UINT mappedVk = 0;
        if ((scanCodeWithFlags & 0xFF) != 0) { mappedVk = MapVirtualKey(scanCodeWithFlags, MAPVK_VSC_TO_VK_EX); }

        DWORD resolvedVk = static_cast<DWORD>(keyWParam);
        if (mappedVk != 0) { resolvedVk = static_cast<DWORD>(mappedVk); }

        // Normalize generic modifier VKs to left/right variants.
        // Windows typically reports VK_CONTROL/VK_MENU/VK_SHIFT in wParam for both sides.
        // For binding UI (hotkeys/rebinds), we want deterministic L/R codes.
        const bool isExtended = (keyLParam & (1LL << 24)) != 0;
        const UINT scanOnly = static_cast<UINT>((keyLParam >> 16) & 0xFF);
        if (resolvedVk == VK_SHIFT) {
            DWORD lr = static_cast<DWORD>(::MapVirtualKeyW(scanOnly, MAPVK_VSC_TO_VK_EX));
            if (lr != 0) { resolvedVk = lr; }
        } else if (resolvedVk == VK_CONTROL) {
            resolvedVk = isExtended ? VK_RCONTROL : VK_LCONTROL;
        } else if (resolvedVk == VK_MENU) {
            resolvedVk = isExtended ? VK_RMENU : VK_LMENU;
        }

        // Ensure dedicated navigation keys keep their non-numpad VK when extended bit is present.
        if ((keyLParam & (1LL << 24)) != 0) {
            switch (scanCodeWithFlags & 0xFF) {
            case 0x4B:
                resolvedVk = VK_LEFT;
                break;
            case 0x4D:
                resolvedVk = VK_RIGHT;
                break;
            case 0x48:
                resolvedVk = VK_UP;
                break;
            case 0x50:
                resolvedVk = VK_DOWN;
                break;
            case 0x47:
                resolvedVk = VK_HOME;
                break;
            case 0x4F:
                resolvedVk = VK_END;
                break;
            case 0x49:
                resolvedVk = VK_PRIOR;
                break;
            case 0x51:
                resolvedVk = VK_NEXT;
                break;
            case 0x52:
                resolvedVk = VK_INSERT;
                break;
            case 0x53:
                resolvedVk = VK_DELETE;
                break;
            default:
                break;
            }
        }

        return resolvedVk;
    };

    switch (uMsg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // Ignore auto-repeat keydown events while binding.
        if ((lParam & (1LL << 30)) != 0) return;
        vk = resolveVkFromKeyboardMessage(wParam, lParam);
        break;
    case WM_LBUTTONDOWN:
        vk = VK_LBUTTON;
        isMouseButton = true;
        break;
    case WM_RBUTTONDOWN:
        vk = VK_RBUTTON;
        isMouseButton = true;
        break;
    case WM_MBUTTONDOWN:
        vk = VK_MBUTTON;
        isMouseButton = true;
        break;
    case WM_XBUTTONDOWN: {
        WORD xButton = GET_XBUTTON_WPARAM(wParam);
        vk = (xButton == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        isMouseButton = true;
        break;
    }
    default:
        return;
    }

    g_bindingInputEventVk.store(vk, std::memory_order_relaxed);
    g_bindingInputEventLParam.store(lParam, std::memory_order_relaxed);
    g_bindingInputEventIsMouse.store(isMouseButton, std::memory_order_relaxed);
    g_bindingInputEventSequence.fetch_add(1, std::memory_order_release);
}

uint64_t GetLatestBindingInputSequence() { return g_bindingInputEventSequence.load(std::memory_order_acquire); }

bool ConsumeBindingInputEventSince(uint64_t& lastSeenSequence, DWORD& outVk, LPARAM& outLParam, bool& outIsMouseButton) {
    uint64_t currentSequence = g_bindingInputEventSequence.load(std::memory_order_acquire);
    if (currentSequence == 0 || currentSequence == lastSeenSequence) { return false; }

    outVk = g_bindingInputEventVk.load(std::memory_order_relaxed);
    outLParam = g_bindingInputEventLParam.load(std::memory_order_relaxed);
    outIsMouseButton = g_bindingInputEventIsMouse.load(std::memory_order_relaxed);
    lastSeenSequence = currentSequence;
    return outVk != 0;
}

// Returns the initial directory for image pickers.
// Prefer Downloads, then fallback to the provided initial directory.
static std::wstring GetImagePickerInitialDirectory(const std::wstring& fallbackInitialDir) {
    PWSTR downloadsPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, NULL, &downloadsPath)) && downloadsPath != nullptr) {
        std::wstring downloads(downloadsPath);
        CoTaskMemFree(downloadsPath);
        if (!downloads.empty() && std::filesystem::exists(downloads)) { return downloads; }
    }

    if (!fallbackInitialDir.empty() && std::filesystem::exists(fallbackInitialDir)) { return fallbackInitialDir; }

    return L"";
}

// Validates an image file by checking if stbi_info can read its header
// Returns empty string on success, or error message on failure
static std::string ValidateImageFile(const std::string& path, const std::wstring& toolscreenPath) {
    if (path.empty()) { return "Path is empty"; }

    // Resolve relative paths
    std::wstring final_path;
    std::wstring image_wpath = Utf8ToWide(path);
    if (PathIsRelativeW(image_wpath.c_str()) && !toolscreenPath.empty()) {
        final_path = toolscreenPath + L"\\" + image_wpath;
    } else {
        final_path = image_wpath;
    }

    // Check file exists
    if (!std::filesystem::exists(final_path)) { return "File does not exist"; }

    std::string path_utf8 = WideToUtf8(final_path);

    // Use stbi_info to check if the file is a valid image without fully loading it
    int w, h, c;
    if (stbi_info(path_utf8.c_str(), &w, &h, &c) == 0) {
        const char* reason = stbi_failure_reason();
        return std::string("Invalid image: ") + (reason ? reason : "unknown format");
    }

    // Basic sanity checks
    if (w <= 0 || h <= 0) { return "Invalid image dimensions"; }

    if (w > 16384 || h > 16384) { return "Image too large (max 16384x16384)"; }

    return ""; // Success
}

// Opens a file picker dialog and validates the selected image
// Returns ImagePickerResult with success/failure and path or error message
static ImagePickerResult OpenImagePickerAndValidate(HWND ownerHwnd, const std::wstring& initialDir, const std::wstring& toolscreenPath) {
    ImagePickerResult result;

    // Check if the owner window is valid and responsive to prevent freezes
    // If the window is not focused or not responding, use NULL as owner
    HWND safeOwner = NULL;
    if (ownerHwnd != NULL && IsWindow(ownerHwnd)) {
        // Check if the window is in the foreground or is the current thread's window
        HWND foreground = GetForegroundWindow();
        DWORD windowThreadId = GetWindowThreadProcessId(ownerHwnd, NULL);
        DWORD currentThreadId = GetCurrentThreadId();

        // Only use the owner if:
        // 1. It's the foreground window, OR
        // 2. It belongs to the current thread (so we can interact with it)
        if (foreground == ownerHwnd || windowThreadId == currentThreadId) { safeOwner = ownerHwnd; }
    }

    OPENFILENAMEW ofn;
    WCHAR szFile[MAX_PATH] = { 0 };
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = safeOwner;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(WCHAR);
    ofn.lpstrFilter =
        L"Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0PNG Files (*.png)\0*.png\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    std::wstring pickerInitialDir = GetImagePickerInitialDirectory(initialDir);
    ofn.lpstrInitialDir = pickerInitialDir.empty() ? NULL : pickerInitialDir.c_str();
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        result.path = WideToUtf8(ofn.lpstrFile);

        // Validate the image before accepting it
        std::string error = ValidateImageFile(result.path, toolscreenPath);
        if (error.empty()) {
            result.success = true;
        } else {
            result.success = false;
            result.error = error;
            result.path = ""; // Clear path on error
        }
    } else {
        // User cancelled - not an error, just no selection
        result.success = false;
        result.error = ""; // No error message for cancellation
    }

    result.completed = true;
    return result;
}

// Clears error message for a given key after timeout (5 seconds)
static void ClearExpiredImageErrors() {
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> keysToRemove;

    for (const auto& [key, time] : g_imageErrorTimes) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - time).count();
        if (elapsed >= 5) { keysToRemove.push_back(key); }
    }

    for (const auto& key : keysToRemove) {
        g_imageErrorMessages.erase(key);
        g_imageErrorTimes.erase(key);
    }
}

// Sets an error message for display
static void SetImageError(const std::string& key, const std::string& error) {
    g_imageErrorMessages[key] = error;
    g_imageErrorTimes[key] = std::chrono::steady_clock::now();
}

// Gets error message for a key (or empty if none/expired)
static std::string GetImageError(const std::string& key) {
    ClearExpiredImageErrors();
    auto it = g_imageErrorMessages.find(key);
    return (it != g_imageErrorMessages.end()) ? it->second : "";
}

// Clears error for a key
static void ClearImageError(const std::string& key) {
    g_imageErrorMessages.erase(key);
    g_imageErrorTimes.erase(key);
}

// Helper to display a little (?) mark which shows a tooltip when hovered
static void HelpMarker(const char* desc) {
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// Helper function to render transition settings in horizontal layout WITHOUT background column (for Fullscreen mode)
static void RenderTransitionSettingsHorizontalNoBackground(ModeConfig& mode, const std::string& idSuffix) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10, 5));

    if (ImGui::BeginTable(("TransitionTableNoBg" + idSuffix).c_str(), 1, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();

        // --- GAME COLUMN ---
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.5f, 0.7f, 0.8f));
        ImGui::Text("Viewport Animation");
        ImGui::PopStyleColor();
        ImGui::Separator();

        ImGui::Text("Type:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* gameOptions[] = { "Cut", "Bounce" };
        int gameType = (mode.gameTransition == GameTransitionType::Cut) ? 0 : 1;
        if (ImGui::Combo(("##GameTrans" + idSuffix).c_str(), &gameType, gameOptions, IM_ARRAYSIZE(gameOptions))) {
            mode.gameTransition = (gameType == 0) ? GameTransitionType::Cut : GameTransitionType::Bounce;
            g_configIsDirty = true;
        }

        if (mode.gameTransition == GameTransitionType::Bounce) {
            ImGui::Spacing();
            ImGui::Text("Duration:");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (Spinner(("##GameDur" + idSuffix).c_str(), &mode.transitionDurationMs, 10, 50, 5000)) { g_configIsDirty = true; }
            ImGui::SameLine();
            ImGui::TextDisabled("ms");

            ImGui::Spacing();
            ImGui::Text("Ease In:");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat(("##EaseIn" + idSuffix).c_str(), &mode.easeInPower, 1.0f, 6.0f, "%.1f")) { g_configIsDirty = true; }

            ImGui::Text("Ease Out:");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat(("##EaseOut" + idSuffix).c_str(), &mode.easeOutPower, 1.0f, 6.0f, "%.1f")) { g_configIsDirty = true; }

            ImGui::Spacing();
            ImGui::Text("Bounces:");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (Spinner(("##BounceCount" + idSuffix).c_str(), &mode.bounceCount, 1, 0, 10)) { g_configIsDirty = true; }

            if (mode.bounceCount > 0) {
                ImGui::Text("Intensity:");
                ImGui::SetNextItemWidth(-FLT_MIN);
                float displayIntensity = mode.bounceIntensity * 100.0f;
                if (ImGui::SliderFloat(("##BounceInt" + idSuffix).c_str(), &displayIntensity, 0.0f, 5.0f, "%.2f")) {
                    mode.bounceIntensity = displayIntensity / 100.0f;
                    g_configIsDirty = true;
                }

                ImGui::Text("Bounce ms:");
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (Spinner(("##BounceDur" + idSuffix).c_str(), &mode.bounceDurationMs, 10, 20, 500)) { g_configIsDirty = true; }
            }

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Checkbox(("Relative Stretching##" + idSuffix).c_str(), &mode.relativeStretching)) { g_configIsDirty = true; }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, viewport-relative overlays scale with the viewport during animation.\nWhen disabled, "
                                  "overlays move with the viewport but keep their original size.");
            }
        }

        // Note: Overlays animate with game during Bounce transitions if relatively positioned

        ImGui::EndTable();
    }

    ImGui::PopStyleVar();

    // Note: No preview button for Fullscreen mode (it's the "from" mode in transitions)
    ImGui::TextDisabled("Note: Fullscreen has no background. Transitions use the other mode's background.");
}

// Helper function to render transition settings in horizontal layout
static void RenderTransitionSettingsHorizontal(ModeConfig& mode, const std::string& idSuffix) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10, 5));

    if (ImGui::BeginTable(("TransitionTable" + idSuffix).c_str(), 1, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Game", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();

        ImGui::TableNextRow();

        // --- GAME COLUMN ---
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.3f, 0.5f, 0.7f, 0.8f));
        ImGui::Text("Viewport Animation");
        ImGui::PopStyleColor();
        ImGui::Separator();

        ImGui::Text("Type:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const char* gameOptions[] = { "Cut", "Bounce" };
        int gameType = (mode.gameTransition == GameTransitionType::Cut) ? 0 : 1;
        if (ImGui::Combo(("##GameTrans" + idSuffix).c_str(), &gameType, gameOptions, IM_ARRAYSIZE(gameOptions))) {
            mode.gameTransition = (gameType == 0) ? GameTransitionType::Cut : GameTransitionType::Bounce;
            g_configIsDirty = true;
        }

        if (mode.gameTransition == GameTransitionType::Bounce) {
            ImGui::Spacing();
            ImGui::Text("Duration:");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (Spinner(("##GameDur" + idSuffix).c_str(), &mode.transitionDurationMs, 10, 50, 5000)) { g_configIsDirty = true; }
            ImGui::SameLine();
            ImGui::TextDisabled("ms");

            ImGui::Spacing();
            ImGui::Text("Ease In:");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat(("##EaseIn" + idSuffix).c_str(), &mode.easeInPower, 1.0f, 6.0f, "%.1f")) { g_configIsDirty = true; }

            ImGui::Text("Ease Out:");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::SliderFloat(("##EaseOut" + idSuffix).c_str(), &mode.easeOutPower, 1.0f, 6.0f, "%.1f")) { g_configIsDirty = true; }

            ImGui::Spacing();
            ImGui::Text("Bounces:");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (Spinner(("##BounceCount" + idSuffix).c_str(), &mode.bounceCount, 1, 0, 10)) { g_configIsDirty = true; }

            if (mode.bounceCount > 0) {
                ImGui::Text("Intensity:");
                ImGui::SetNextItemWidth(-FLT_MIN);
                float displayIntensity = mode.bounceIntensity * 100.0f;
                if (ImGui::SliderFloat(("##BounceInt" + idSuffix).c_str(), &displayIntensity, 0.0f, 5.0f, "%.2f")) {
                    mode.bounceIntensity = displayIntensity / 100.0f;
                    g_configIsDirty = true;
                }

                ImGui::Text("Bounce ms:");
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (Spinner(("##BounceDur" + idSuffix).c_str(), &mode.bounceDurationMs, 10, 20, 500)) { g_configIsDirty = true; }
            }

            ImGui::Spacing();
            ImGui::Separator();
            if (ImGui::Checkbox(("Relative Stretching##" + idSuffix).c_str(), &mode.relativeStretching)) { g_configIsDirty = true; }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, viewport-relative overlays scale with the viewport during animation.\nWhen disabled, "
                                  "overlays move with the viewport but keep their original size.");
            }

            // Skip axis animation options
            if (ImGui::Checkbox(("Skip X Animation##" + idSuffix).c_str(), &mode.skipAnimateX)) { g_configIsDirty = true; }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, the X axis (width) instantly jumps to target while Y animates.");
            }

            if (ImGui::Checkbox(("Skip Y Animation##" + idSuffix).c_str(), &mode.skipAnimateY)) { g_configIsDirty = true; }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("When enabled, the Y axis (height) instantly jumps to target while X animates.");
            }
        }

        // Note: Overlays animate with game during Bounce transitions if relatively positioned
        // Background transitions are always instant (Cut)

        ImGui::EndTable();
    }

    ImGui::PopStyleVar();

    // Preview button
    ImGui::Spacing();
    if (ImGui::Button(("Preview Transition##" + idSuffix).c_str())) {
        std::lock_guard<std::mutex> pendingLock(g_pendingModeSwitchMutex);
        g_pendingModeSwitch.pending = true;
        g_pendingModeSwitch.isPreview = true;
        g_pendingModeSwitch.previewFromModeId = "Fullscreen";
        g_pendingModeSwitch.modeId = mode.id;
        g_pendingModeSwitch.source = "Preview button";
        Log("[GUI] Queued transition preview: Fullscreen -> " + mode.id);
    }
    ImGui::SameLine();
    HelpMarker("Preview the transition by switching from Fullscreen to this mode.");
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
    return GameTransitionType::Bounce; // Default to Bounce
}

std::string OverlayTransitionTypeToString(OverlayTransitionType type) {
    switch (type) {
    case OverlayTransitionType::Cut:
        return "Cut";
    default:
        return "Cut";
    }
}

OverlayTransitionType StringToOverlayTransitionType(const std::string& str) {
    return OverlayTransitionType::Cut; // Only Cut is supported
}

std::string BackgroundTransitionTypeToString(BackgroundTransitionType type) {
    switch (type) {
    case BackgroundTransitionType::Cut:
        return "Cut";
    default:
        return "Cut";
    }
}

BackgroundTransitionType StringToBackgroundTransitionType(const std::string& str) {
    return BackgroundTransitionType::Cut; // Only Cut is supported
}

std::string& ToUpper(std::string& s) {

    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::string VkToString(DWORD vk) {
    if (vk == 0) return "[None]";

    static std::map<DWORD, std::string> specialKeys = { { VK_LBUTTON, "MOUSE1" },
                                                        { VK_RBUTTON, "MOUSE2" },
                                                        { VK_MBUTTON, "MOUSE3" },
                                                        { VK_XBUTTON1, "MOUSE4" },
                                                        { VK_XBUTTON2, "MOUSE5" },
                                                        { VK_SHIFT, "SHIFT" },
                                                        { VK_LSHIFT, "LSHIFT" },
                                                        { VK_RSHIFT, "RSHIFT" },
                                                        { VK_CONTROL, "CTRL" },
                                                        { VK_LCONTROL, "LCTRL" },
                                                        { VK_RCONTROL, "RCTRL" },
                                                        { VK_MENU, "ALT" },
                                                        { VK_LMENU, "LALT" },
                                                        { VK_RMENU, "RALT" },
                                                        { VK_LWIN, "LWIN" },
                                                        { VK_RWIN, "RWIN" },
                                                        { VK_F1, "F1" },
                                                        { VK_F2, "F2" },
                                                        { VK_F3, "F3" },
                                                        { VK_F4, "F4" },
                                                        { VK_F5, "F5" },
                                                        { VK_F6, "F6" },
                                                        { VK_F7, "F7" },
                                                        { VK_F8, "F8" },
                                                        { VK_F9, "F9" },
                                                        { VK_F10, "F10" },
                                                        { VK_F11, "F11" },
                                                        { VK_F12, "F12" },
                                                        { VK_F13, "F13" },
                                                        { VK_F14, "F14" },
                                                        { VK_F15, "F15" },
                                                        { VK_F16, "F16" },
                                                        { VK_F17, "F17" },
                                                        { VK_F18, "F18" },
                                                        { VK_F19, "F19" },
                                                        { VK_F20, "F20" },
                                                        { VK_F21, "F21" },
                                                        { VK_F22, "F22" },
                                                        { VK_F23, "F23" },
                                                        { VK_F24, "F24" },
                                                        { VK_BACK, "BACKSPACE" },
                                                        { VK_TAB, "TAB" },
                                                        { VK_RETURN, "ENTER" },
                                                        { VK_CAPITAL, "CAPS LOCK" },
                                                        { VK_ESCAPE, "ESC" },
                                                        { VK_SPACE, "SPACE" },
                                                        { VK_PRIOR, "PAGE UP" },
                                                        { VK_NEXT, "PAGE DOWN" },
                                                        { VK_END, "END" },
                                                        { VK_HOME, "HOME" },
                                                        { VK_LEFT, "LEFT" },
                                                        { VK_UP, "UP" },
                                                        { VK_RIGHT, "RIGHT" },
                                                        { VK_DOWN, "DOWN" },
                                                        { VK_INSERT, "INSERT" },
                                                        { VK_DELETE, "DELETE" },
                                                        { VK_NUMPAD0, "NUM 0" },
                                                        { VK_NUMPAD1, "NUM 1" },
                                                        { VK_NUMPAD2, "NUM 2" },
                                                        { VK_NUMPAD3, "NUM 3" },
                                                        { VK_NUMPAD4, "NUM 4" },
                                                        { VK_NUMPAD5, "NUM 5" },
                                                        { VK_NUMPAD6, "NUM 6" },
                                                        { VK_NUMPAD7, "NUM 7" },
                                                        { VK_NUMPAD8, "NUM 8" },
                                                        { VK_NUMPAD9, "NUM 9" },
                                                        { VK_MULTIPLY, "NUM *" },
                                                        { VK_ADD, "NUM +" },
                                                        { VK_SEPARATOR, "NUM SEP" },
                                                        { VK_SUBTRACT, "NUM -" },
                                                        { VK_DECIMAL, "NUM ." },
                                                        { VK_DIVIDE, "NUM /" },
                                                        { VK_OEM_1, ";" },
                                                        { VK_OEM_PLUS, "=" },
                                                        { VK_OEM_COMMA, "," },
                                                        { VK_OEM_MINUS, "-" },
                                                        { VK_OEM_PERIOD, "." },
                                                        { VK_OEM_2, "/" },
                                                        { VK_OEM_3, "`" },
                                                        { VK_OEM_4, "[" },
                                                        { VK_OEM_5, "\\" },
                                                        { VK_OEM_6, "]" },
                                                        { VK_OEM_7, "'" } };
    if (specialKeys.count(vk)) { return specialKeys.at(vk); }

    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { return std::string(1, (char)vk); }

    char keyName[128];
    if (GetKeyNameTextA(MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) << 16, keyName, sizeof(keyName)) != 0) {
        std::string str(keyName);
        return ToUpper(str);
    }

    std::stringstream ss;
    ss << "0x" << std::hex << std::uppercase << vk;
    return ss.str();
}

DWORD StringToVk(const std::string& keyStr) {
    std::string cleanKey = keyStr;

    // Remove leading/trailing whitespace
    cleanKey.erase(0, cleanKey.find_first_not_of(" \t\r\n"));
    cleanKey.erase(cleanKey.find_last_not_of(" \t\r\n") + 1);

    // Convert to uppercase for comparison
    ToUpper(cleanKey);

    // Handle empty or whitespace-only strings
    if (cleanKey.empty()) return 0;

    // clang-format off
    // Extended key mapping with alternative names
    static std::map<std::string, DWORD> keyMap = {
        // Mouse buttons
        {"MOUSE1", VK_LBUTTON}, {"LBUTTON", VK_LBUTTON}, {"LEFTMOUSE", VK_LBUTTON},
        {"MOUSE2", VK_RBUTTON}, {"RBUTTON", VK_RBUTTON}, {"RIGHTMOUSE", VK_RBUTTON},
        {"MOUSE3", VK_MBUTTON}, {"MBUTTON", VK_MBUTTON}, {"MIDDLEMOUSE", VK_MBUTTON},
        {"MOUSE4", VK_XBUTTON1}, {"XBUTTON1", VK_XBUTTON1}, {"MOUSE BUTTON 4", VK_XBUTTON1}, {"MOUSEBUTTON4", VK_XBUTTON1},
        {"MOUSE5", VK_XBUTTON2}, {"XBUTTON2", VK_XBUTTON2}, {"MOUSE BUTTON 5", VK_XBUTTON2}, {"MOUSEBUTTON5", VK_XBUTTON2},
        
        // Modifier keys
        {"SHIFT", VK_SHIFT}, {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT},
        {"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL}, {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL},
        {"LCONTROL", VK_LCONTROL}, {"RCONTROL", VK_RCONTROL},
        {"ALT", VK_MENU}, {"MENU", VK_MENU}, {"LALT", VK_LMENU}, {"RALT", VK_RMENU}, {"LMENU", VK_LMENU}, {"RMENU", VK_RMENU},
        {"WIN", VK_LWIN}, {"WINDOWS", VK_LWIN}, {"LWIN", VK_LWIN}, {"RWIN", VK_RWIN}, {"WINKEY", VK_LWIN}, {"WINDOWSKEY", VK_LWIN},
        
        // Function keys
        {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4}, {"F5", VK_F5}, {"F6", VK_F6},
        {"F7", VK_F7}, {"F8", VK_F8}, {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
        {"F13", VK_F13}, {"F14", VK_F14}, {"F15", VK_F15}, {"F16", VK_F16}, {"F17", VK_F17}, {"F18", VK_F18},
        {"F19", VK_F19}, {"F20", VK_F20}, {"F21", VK_F21}, {"F22", VK_F22}, {"F23", VK_F23}, {"F24", VK_F24},
        
        // Special keys with multiple names
        {"BACKSPACE", VK_BACK}, {"BACK", VK_BACK}, {"BKSP", VK_BACK},
        {"TAB", VK_TAB}, {"TABULATOR", VK_TAB},
        {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN}, {"CR", VK_RETURN},
        {"CAPS LOCK", VK_CAPITAL}, {"CAPSLOCK", VK_CAPITAL}, {"CAPS", VK_CAPITAL}, {"CAPITAL", VK_CAPITAL},
        {"ESCAPE", VK_ESCAPE}, {"ESC", VK_ESCAPE},
        {"SPACE", VK_SPACE}, {"SPACEBAR", VK_SPACE}, {"SPC", VK_SPACE},
        
        // Navigation keys
        {"PAGE UP", VK_PRIOR}, {"PAGEUP", VK_PRIOR}, {"PGUP", VK_PRIOR}, {"PRIOR", VK_PRIOR},
        {"PAGE DOWN", VK_NEXT}, {"PAGEDOWN", VK_NEXT}, {"PGDN", VK_NEXT}, {"NEXT", VK_NEXT},
        {"END", VK_END}, {"HOME", VK_HOME},
        {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"UP", VK_UP}, {"DOWN", VK_DOWN},
        {"ARROW LEFT", VK_LEFT}, {"ARROWLEFT", VK_LEFT}, {"LEFT ARROW", VK_LEFT}, {"LEFTARROW", VK_LEFT},
        {"ARROW RIGHT", VK_RIGHT}, {"ARROWRIGHT", VK_RIGHT}, {"RIGHT ARROW", VK_RIGHT}, {"RIGHTARROW", VK_RIGHT},
        {"ARROW UP", VK_UP}, {"ARROWUP", VK_UP}, {"UP ARROW", VK_UP}, {"UPARROW", VK_UP},
        {"ARROW DOWN", VK_DOWN}, {"ARROWDOWN", VK_DOWN}, {"DOWN ARROW", VK_DOWN}, {"DOWNARROW", VK_DOWN},
        {"INSERT", VK_INSERT}, {"INS", VK_INSERT},
        {"DELETE", VK_DELETE}, {"DEL", VK_DELETE},
        
        // Numpad keys
        {"NUMPAD 0", VK_NUMPAD0}, {"NUMPAD0", VK_NUMPAD0}, {"NUM 0", VK_NUMPAD0}, {"NUM0", VK_NUMPAD0},
        {"NUMPAD 1", VK_NUMPAD1}, {"NUMPAD1", VK_NUMPAD1}, {"NUM 1", VK_NUMPAD1}, {"NUM1", VK_NUMPAD1},
        {"NUMPAD 2", VK_NUMPAD2}, {"NUMPAD2", VK_NUMPAD2}, {"NUM 2", VK_NUMPAD2}, {"NUM2", VK_NUMPAD2},
        {"NUMPAD 3", VK_NUMPAD3}, {"NUMPAD3", VK_NUMPAD3}, {"NUM 3", VK_NUMPAD3}, {"NUM3", VK_NUMPAD3},
        {"NUMPAD 4", VK_NUMPAD4}, {"NUMPAD4", VK_NUMPAD4}, {"NUM 4", VK_NUMPAD4}, {"NUM4", VK_NUMPAD4},
        {"NUMPAD 5", VK_NUMPAD5}, {"NUMPAD5", VK_NUMPAD5}, {"NUM 5", VK_NUMPAD5}, {"NUM5", VK_NUMPAD5},
        {"NUMPAD 6", VK_NUMPAD6}, {"NUMPAD6", VK_NUMPAD6}, {"NUM 6", VK_NUMPAD6}, {"NUM6", VK_NUMPAD6},
        {"NUMPAD 7", VK_NUMPAD7}, {"NUMPAD7", VK_NUMPAD7}, {"NUM 7", VK_NUMPAD7}, {"NUM7", VK_NUMPAD7},
        {"NUMPAD 8", VK_NUMPAD8}, {"NUMPAD8", VK_NUMPAD8}, {"NUM 8", VK_NUMPAD8}, {"NUM8", VK_NUMPAD8},
        {"NUMPAD 9", VK_NUMPAD9}, {"NUMPAD9", VK_NUMPAD9}, {"NUM 9", VK_NUMPAD9}, {"NUM9", VK_NUMPAD9},
        {"NUMPAD *", VK_MULTIPLY}, {"NUMPAD*", VK_MULTIPLY}, {"NUM *", VK_MULTIPLY}, {"NUM*", VK_MULTIPLY},
        {"NUMPAD +", VK_ADD}, {"NUMPAD+", VK_ADD}, {"NUM +", VK_ADD}, {"NUM+", VK_ADD},
        {"NUMPAD -", VK_SUBTRACT}, {"NUMPAD-", VK_SUBTRACT}, {"NUM -", VK_SUBTRACT}, {"NUM-", VK_SUBTRACT},
        {"NUMPAD .", VK_DECIMAL}, {"NUMPAD.", VK_DECIMAL}, {"NUM .", VK_DECIMAL}, {"NUM.", VK_DECIMAL},
        {"NUMPAD /", VK_DIVIDE}, {"NUMPAD/", VK_DIVIDE}, {"NUM /", VK_DIVIDE}, {"NUM/", VK_DIVIDE},
        {"NUMPAD SEP", VK_SEPARATOR}, {"NUMPADSEP", VK_SEPARATOR}, {"NUM SEP", VK_SEPARATOR}, {"NUMSEP", VK_SEPARATOR},
        
        // Symbols and punctuation
        {";", VK_OEM_1}, {"SEMICOLON", VK_OEM_1},
        {"=", VK_OEM_PLUS}, {"EQUALS", VK_OEM_PLUS}, {"PLUS", VK_OEM_PLUS},
        {",", VK_OEM_COMMA}, {"COMMA", VK_OEM_COMMA},
        {"-", VK_OEM_MINUS}, {"MINUS", VK_OEM_MINUS}, {"DASH", VK_OEM_MINUS}, {"HYPHEN", VK_OEM_MINUS},
        {".", VK_OEM_PERIOD}, {"PERIOD", VK_OEM_PERIOD}, {"DOT", VK_OEM_PERIOD},
        {"/", VK_OEM_2}, {"SLASH", VK_OEM_2}, {"FORWARDSLASH", VK_OEM_2},
        {"`", VK_OEM_3}, {"GRAVE", VK_OEM_3}, {"BACKTICK", VK_OEM_3}, {"TILDE", VK_OEM_3},
        {"[", VK_OEM_4}, {"LEFTBRACKET", VK_OEM_4}, {"OPENBRACKET", VK_OEM_4},
        {"\\", VK_OEM_5}, {"BACKSLASH", VK_OEM_5},
        {"]", VK_OEM_6}, {"RIGHTBRACKET", VK_OEM_6}, {"CLOSEBRACKET", VK_OEM_6},
        {"'", VK_OEM_7}, {"QUOTE", VK_OEM_7}, {"APOSTROPHE", VK_OEM_7}, {"SINGLEQUOTE", VK_OEM_7},
        
        // Lock keys
        {"SCROLL LOCK", VK_SCROLL}, {"SCROLLLOCK", VK_SCROLL}, {"SCROLL", VK_SCROLL},
        {"NUM LOCK", VK_NUMLOCK}, {"NUMLOCK", VK_NUMLOCK},
        
        // Other keys
        {"PRINT SCREEN", VK_SNAPSHOT}, {"PRINTSCREEN", VK_SNAPSHOT}, {"PRTSC", VK_SNAPSHOT}, {"SNAPSHOT", VK_SNAPSHOT},
        {"PAUSE", VK_PAUSE}, {"BREAK", VK_PAUSE}, {"PAUSE BREAK", VK_PAUSE}, {"PAUSEBREAK", VK_PAUSE},
        {"APPS", VK_APPS}, {"APPLICATION", VK_APPS}, {"CONTEXT", VK_APPS}, {"CONTEXTMENU", VK_APPS}
    };
    // clang-format on

    // Check direct mapping first
    if (keyMap.count(cleanKey)) { return keyMap[cleanKey]; }

    // Handle single character keys
    if (cleanKey.length() == 1) {
        char c = cleanKey[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) { return (DWORD)c; }
        // For other single characters, use VkKeyScan
        return VkKeyScanA(c) & 0xFF;
    }

    // Handle hex values (0x format or just hex)
    if (cleanKey.rfind("0X", 0) == 0 && cleanKey.length() > 2) {
        try {
            return std::stoul(cleanKey.substr(2), nullptr, 16);
        } catch (...) { return 0; }
    }

    // Handle decimal values if it's all digits
    if (std::all_of(cleanKey.begin(), cleanKey.end(), ::isdigit)) {
        try {
            DWORD val = std::stoul(cleanKey);
            return (val > 0 && val < 256) ? val : 0;
        } catch (...) { return 0; }
    }

    return 0;
}

// Enhanced hotkey parsing function that handles various formats
std::vector<DWORD> ParseHotkeyString(const std::string& hotkeyStr) {
    std::vector<DWORD> keys;

    if (hotkeyStr.empty()) return keys;

    std::string cleanStr = hotkeyStr;

    // Remove leading/trailing whitespace
    cleanStr.erase(0, cleanStr.find_first_not_of(" \t\r\n"));
    cleanStr.erase(cleanStr.find_last_not_of(" \t\r\n") + 1);

    if (cleanStr.empty()) return keys;

    // Define multiple possible separators
    std::vector<char> separators = { '+', '-', '_', ',', '|', '&' };
    char usedSeparator = '\0';

    // Find which separator is used (prefer + if found)
    for (char sep : separators) {
        if (cleanStr.find(sep) != std::string::npos) {
            usedSeparator = sep;
            if (sep == '+') break; // Prefer + separator
        }
    }

    // Split the string
    std::vector<std::string> keyParts;
    if (usedSeparator != '\0') {
        // Split by separator
        std::stringstream ss(cleanStr);
        std::string keyPart;
        while (std::getline(ss, keyPart, usedSeparator)) { keyParts.push_back(keyPart); }
    } else {
        // Check if it contains spaces and might be space-separated
        if (cleanStr.find(' ') != std::string::npos) {
            // Try splitting by spaces, but be careful about key names with spaces
            std::stringstream ss(cleanStr);
            std::string word;
            std::string current_key;

            while (ss >> word) {
                if (!current_key.empty()) {
                    // Try to parse current accumulated key
                    DWORD testVk = StringToVk(current_key);
                    if (testVk != 0) {
                        // Current key is valid, start new key with this word
                        keyParts.push_back(current_key);
                        current_key = word;
                    } else {
                        // Current key not valid yet, add this word to it
                        current_key += " " + word;
                    }
                } else {
                    current_key = word;
                }
            }

            // Add the last accumulated key
            if (!current_key.empty()) { keyParts.push_back(current_key); }
        } else {
            // Single key
            keyParts.push_back(cleanStr);
        }
    }

    // Parse each key part
    for (const std::string& keyPart : keyParts) {
        DWORD vk = StringToVk(keyPart);
        if (vk != 0) { keys.push_back(vk); }
    }

    return keys;
}

ImGuiKey VkToImGuiKey(int vk) {
    switch (vk) {
    case VK_TAB:
        return ImGuiKey_Tab;
    case VK_LEFT:
        return ImGuiKey_LeftArrow;
    case VK_RIGHT:
        return ImGuiKey_RightArrow;
    case VK_UP:
        return ImGuiKey_UpArrow;
    case VK_DOWN:
        return ImGuiKey_DownArrow;
    case VK_PRIOR:
        return ImGuiKey_PageUp;
    case VK_NEXT:
        return ImGuiKey_PageDown;
    case VK_HOME:
        return ImGuiKey_Home;
    case VK_END:
        return ImGuiKey_End;
    case VK_INSERT:
        return ImGuiKey_Insert;
    case VK_DELETE:
        return ImGuiKey_Delete;
    case VK_BACK:
        return ImGuiKey_Backspace;
    case VK_SPACE:
        return ImGuiKey_Space;
    case VK_RETURN:
        return ImGuiKey_Enter;
    case VK_ESCAPE:
        return ImGuiKey_Escape;
    case VK_OEM_7:
        return ImGuiKey_Apostrophe;
    case VK_OEM_COMMA:
        return ImGuiKey_Comma;
    case VK_OEM_MINUS:
        return ImGuiKey_Minus;
    case VK_OEM_PERIOD:
        return ImGuiKey_Period;
    case VK_OEM_2:
        return ImGuiKey_Slash;
    case VK_OEM_1:
        return ImGuiKey_Semicolon;
    case VK_OEM_PLUS:
        return ImGuiKey_Equal;
    case VK_OEM_4:
        return ImGuiKey_LeftBracket;
    case VK_OEM_5:
        return ImGuiKey_Backslash;
    case VK_OEM_6:
        return ImGuiKey_RightBracket;
    case VK_OEM_3:
        return ImGuiKey_GraveAccent;
    case VK_CAPITAL:
        return ImGuiKey_CapsLock;
    case VK_SCROLL:
        return ImGuiKey_ScrollLock;
    case VK_NUMLOCK:
        return ImGuiKey_NumLock;
    case VK_SNAPSHOT:
        return ImGuiKey_PrintScreen;
    case VK_PAUSE:
        return ImGuiKey_Pause;
    case VK_NUMPAD0:
        return ImGuiKey_Keypad0;
    case VK_NUMPAD1:
        return ImGuiKey_Keypad1;
    case VK_NUMPAD2:
        return ImGuiKey_Keypad2;
    case VK_NUMPAD3:
        return ImGuiKey_Keypad3;
    case VK_NUMPAD4:
        return ImGuiKey_Keypad4;
    case VK_NUMPAD5:
        return ImGuiKey_Keypad5;
    case VK_NUMPAD6:
        return ImGuiKey_Keypad6;
    case VK_NUMPAD7:
        return ImGuiKey_Keypad7;
    case VK_NUMPAD8:
        return ImGuiKey_Keypad8;
    case VK_NUMPAD9:
        return ImGuiKey_Keypad9;
    case VK_DECIMAL:
        return ImGuiKey_KeypadDecimal;
    case VK_DIVIDE:
        return ImGuiKey_KeypadDivide;
    case VK_MULTIPLY:
        return ImGuiKey_KeypadMultiply;
    case VK_SUBTRACT:
        return ImGuiKey_KeypadSubtract;
    case VK_ADD:
        return ImGuiKey_KeypadAdd;
    case VK_F1:
        return ImGuiKey_F1;
    case VK_F2:
        return ImGuiKey_F2;
    case VK_F3:
        return ImGuiKey_F3;
    case VK_F4:
        return ImGuiKey_F4;
    case VK_F5:
        return ImGuiKey_F5;
    case VK_F6:
        return ImGuiKey_F6;
    case VK_F7:
        return ImGuiKey_F7;
    case VK_F8:
        return ImGuiKey_F8;
    case VK_F9:
        return ImGuiKey_F9;
    case VK_F10:
        return ImGuiKey_F10;
    case VK_F11:
        return ImGuiKey_F11;
    case VK_F12:
        return ImGuiKey_F12;
    }
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
    return ImGuiKey_None;
}

void CopyToClipboard(HWND hwnd, const std::string& text) {
    if (!OpenClipboard(hwnd)) {
        Log("ERROR: Could not open clipboard. Error code: " + std::to_string(GetLastError()));
        return;
    }

    // RAII wrapper for closing clipboard
    struct ClipboardGuard {
        ClipboardGuard() {}
        ~ClipboardGuard() { CloseClipboard(); }
    } guard;

    if (!EmptyClipboard()) {
        Log("ERROR: Could not empty clipboard. Error code: " + std::to_string(GetLastError()));
        return;
    }

    std::wstring wide_text = Utf8ToWide(text);
    size_t size = (wide_text.length() + 1) * sizeof(WCHAR);

    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hg) {
        Log("ERROR: GlobalAlloc failed. Error code: " + std::to_string(GetLastError()));
        return;
    }

    void* pGlobal = GlobalLock(hg);
    if (!pGlobal) {
        Log("ERROR: GlobalLock failed. Error code: " + std::to_string(GetLastError()));
        GlobalFree(hg);
        return;
    }

    memcpy(pGlobal, wide_text.c_str(), size);
    GlobalUnlock(hg);

    if (!SetClipboardData(CF_UNICODETEXT, hg)) {
        Log("ERROR: SetClipboardData failed. Error code: " + std::to_string(GetLastError()));
        GlobalFree(hg); // We own the memory if SetClipboardData fails
    }
}

// This function MUST be defined before the JSON serialization functions that call it
EyeZoomConfig GetDefaultEyeZoomConfig() { return GetDefaultEyeZoomConfigFromEmbedded(); }

void ParseColorString(const std::string& input, Color& outColor) {
    std::string s = input;
    s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
    if (s.rfind('#', 0) == 0) s = s.substr(1);
    if (s.length() == 6 && s.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos) {
        try {
            unsigned long value = std::stoul(s, nullptr, 16);
            outColor = { ((value >> 16) & 0xFF) / 255.0f, ((value >> 8) & 0xFF) / 255.0f, (value & 0xFF) / 255.0f };
            return;
        } catch (...) {}
    }
    std::stringstream ss(s);
    std::string item;
    float components[3];
    int i = 0;
    while (std::getline(ss, item, ',') && i < 3) {
        try {
            components[i++] = std::stof(item);
        } catch (...) { goto error_case; }
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

    // Throttle saves: only save if config is dirty AND at least 1 second has passed
    static auto s_lastSaveTime = std::chrono::steady_clock::now();

    auto currentTime = std::chrono::steady_clock::now();
    auto timeSinceLastSave = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - s_lastSaveTime).count();

    if (!g_configIsDirty.load()) {
        return; // Nothing changed, skip save
    }

    if (timeSinceLastSave < 1000) {
        return; // Less than 1 second since last save, skip
    }

    if (s_isConfigSaving.load()) {
        return; // Save in progress, skip
    }

    if (g_toolscreenPath.empty()) {
        Log("ERROR: Cannot save config, toolscreen path is not available.");
        return;
    }
    std::wstring configPath = g_toolscreenPath + L"\\config.toml";
    try {
        // Snapshot config and convert to TOML (thread-safe for detached write thread)
        toml::table tbl;
        ConfigToToml(g_config, tbl);

        // Publish updated config snapshot for reader threads (RCU pattern)
        PublishConfigSnapshot();

        g_configIsDirty = false;
        s_lastSaveTime = currentTime;
        s_isConfigSaving = true;

        std::thread([configPath, tbl = std::move(tbl)]() {
            _set_se_translator(SEHTranslator);
            try {
                try {
                    std::string narrowPath = WideToUtf8(configPath);
                    std::ofstream o(narrowPath);
                    if (!o.is_open()) {
                        Log("ERROR: Failed to open config file for writing.");
                    } else {
                        o << tbl;
                        o.close();
                    }
                } catch (const std::exception& e) { Log("ERROR: Failed to write config file: " + std::string(e.what())); }
            } catch (const SE_Exception& e) {
                LogException("ConfigSaveThread (SEH)", e.getCode(), e.getInfo());
            } catch (const std::exception& e) { LogException("ConfigSaveThread", e); } catch (...) {
                Log("EXCEPTION in ConfigSaveThread: Unknown exception");
            }
            s_isConfigSaving = false;
        }).detach();

    } catch (const std::exception& e) { Log("ERROR: Failed to prepare config for save: " + std::string(e.what())); } catch (...) {
        Log("ERROR: Unknown exception in SaveConfig");
    }
}

// Force immediate save, bypassing throttle (for shutdown, GUI close, etc.)
void SaveConfigImmediate() {
    PROFILE_SCOPE_CAT("Config Save (Immediate)", "IO Operations");

    // Wait for any background save to complete to avoid file corruption
    if (s_isConfigSaving.load()) {
        Log("SaveConfigImmediate: Waiting for background save to complete...");
        auto startWait = std::chrono::steady_clock::now();
        while (s_isConfigSaving.load()) {
            // Timeout after 3 seconds to prevent infinite hang on shutdown
            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startWait).count() > 3) {
                Log("SaveConfigImmediate: Timed out waiting for background save. Proceeding anyway.");
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    if (!g_configIsDirty) {
        return; // Nothing changed, skip save
    }

    if (g_toolscreenPath.empty()) {
        Log("ERROR: Cannot save config, toolscreen path is not available.");
        return;
    }
    std::wstring configPath = g_toolscreenPath + L"\\config.toml";
    try {
        Log("SaveConfigImmediate: Starting config copy...");
        // Convert config to TOML
        toml::table tbl;
        ConfigToToml(g_config, tbl);

        // Publish updated config snapshot for reader threads (RCU pattern)
        PublishConfigSnapshot();

        std::string narrowPath = WideToUtf8(configPath);
        std::ofstream o(narrowPath);
        if (!o.is_open()) {
            Log("ERROR: Failed to open config file for writing.");
            return;
        }
        o << tbl;
        o.close();

        Log("Configuration saved to file (immediate).");
        g_configIsDirty = false;

        // Cursor config is now handled directly by fake_cursor.cpp
    } catch (const std::exception& e) { Log("ERROR: Failed to write config file: " + std::string(e.what())); } catch (...) {
        Log("ERROR: Unknown exception in SaveConfigImmediate");
    }
}

// Helper to apply a preset theme's colors
static void ApplyPresetThemeColors(const std::string& themeName) {
    ImGuiStyle& style = ImGui::GetStyle();

    if (themeName == "Dracula") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.16f, 0.16f, 0.21f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.27f, 0.29f, 0.40f, 1.00f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.97f, 0.98f, 0.98f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.38f, 0.42f, 0.53f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.27f, 0.29f, 0.40f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.35f, 0.38f, 0.53f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.55f, 0.48f, 0.76f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.16f, 0.21f, 1.00f);
        style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.16f, 0.16f, 0.21f, 0.51f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.55f, 0.48f, 0.76f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.55f, 0.48f, 0.76f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.98f, 0.47f, 0.60f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.55f, 0.48f, 0.76f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.55f, 0.48f, 0.76f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.55f, 0.48f, 0.76f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.27f, 0.29f, 0.40f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.55f, 0.48f, 0.76f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.55f, 0.48f, 0.76f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.55f, 0.48f, 0.76f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.98f, 0.47f, 0.60f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.31f, 0.98f, 0.48f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.27f, 0.29f, 0.40f, 1.00f);
    } else if (themeName == "Nord") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.18f, 0.20f, 0.25f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.26f, 0.30f, 0.37f, 1.00f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.93f, 0.94f, 0.96f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.43f, 0.47f, 0.55f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.26f, 0.30f, 0.37f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.33f, 0.43f, 0.58f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.53f, 0.75f, 0.82f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.18f, 0.20f, 0.25f, 1.00f);
        style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.18f, 0.20f, 0.25f, 0.51f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.33f, 0.43f, 0.58f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.69f, 0.76f, 0.82f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.33f, 0.43f, 0.58f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.53f, 0.75f, 0.82f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.26f, 0.30f, 0.37f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.53f, 0.75f, 0.82f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.53f, 0.75f, 0.82f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.69f, 0.76f, 0.82f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.64f, 0.83f, 0.64f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.26f, 0.30f, 0.37f, 1.00f);
    } else if (themeName == "Solarized") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.17f, 0.21f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.17f, 0.21f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.00f, 0.17f, 0.21f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.35f, 0.43f, 0.46f, 0.50f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.51f, 0.58f, 0.59f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.35f, 0.43f, 0.46f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.03f, 0.21f, 0.26f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.15f, 0.55f, 0.67f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.15f, 0.55f, 0.67f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.17f, 0.21f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.00f, 0.17f, 0.21f, 1.00f);
        style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.17f, 0.21f, 0.51f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.55f, 0.67f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.15f, 0.55f, 0.67f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.52f, 0.60f, 0.00f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.15f, 0.55f, 0.67f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.15f, 0.55f, 0.67f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.15f, 0.55f, 0.67f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.03f, 0.21f, 0.26f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.15f, 0.55f, 0.67f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.15f, 0.55f, 0.67f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.15f, 0.55f, 0.67f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.52f, 0.60f, 0.00f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.52f, 0.60f, 0.00f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.35f, 0.43f, 0.46f, 0.50f);
    } else if (themeName == "Monokai") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.15f, 0.15f, 0.13f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.15f, 0.15f, 0.13f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.15f, 0.15f, 0.13f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.46f, 0.44f, 0.37f, 0.50f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.97f, 0.97f, 0.95f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.46f, 0.44f, 0.37f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.23f, 0.23f, 0.20f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.40f, 0.88f, 0.33f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.40f, 0.88f, 0.33f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.15f, 0.15f, 0.13f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.15f, 0.13f, 1.00f);
        style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.15f, 0.15f, 0.13f, 0.51f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.98f, 0.15f, 0.45f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.98f, 0.15f, 0.45f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.88f, 0.33f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.98f, 0.15f, 0.45f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.98f, 0.15f, 0.45f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.98f, 0.15f, 0.45f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.23f, 0.23f, 0.20f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.98f, 0.15f, 0.45f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.98f, 0.15f, 0.45f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.88f, 0.33f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.98f, 0.89f, 0.36f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.40f, 0.88f, 0.33f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.46f, 0.44f, 0.37f, 0.50f);
    } else if (themeName == "Catppuccin") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.12f, 0.18f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.27f, 0.28f, 0.35f, 1.00f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.81f, 0.84f, 0.96f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.42f, 0.44f, 0.53f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.17f, 0.18f, 0.25f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.53f, 0.56f, 0.89f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.53f, 0.56f, 0.89f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
        style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.12f, 0.12f, 0.18f, 0.51f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.53f, 0.56f, 0.89f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.53f, 0.56f, 0.89f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.95f, 0.55f, 0.66f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.53f, 0.56f, 0.89f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.53f, 0.56f, 0.89f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.53f, 0.56f, 0.89f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.17f, 0.18f, 0.25f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.53f, 0.56f, 0.89f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.53f, 0.56f, 0.89f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.53f, 0.56f, 0.89f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.95f, 0.55f, 0.66f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.65f, 0.89f, 0.63f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.27f, 0.28f, 0.35f, 1.00f);
    } else if (themeName == "One Dark") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.16f, 0.18f, 0.21f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.16f, 0.18f, 0.21f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.16f, 0.18f, 0.21f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.28f, 0.31f, 0.36f, 1.00f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.67f, 0.73f, 0.82f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.39f, 0.42f, 0.47f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.21f, 0.24f, 0.28f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.38f, 0.53f, 0.87f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.38f, 0.53f, 0.87f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.16f, 0.18f, 0.21f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.18f, 0.21f, 1.00f);
        style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.16f, 0.18f, 0.21f, 0.51f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.38f, 0.53f, 0.87f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.38f, 0.53f, 0.87f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.20f, 0.80f, 0.62f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.38f, 0.53f, 0.87f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.38f, 0.53f, 0.87f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.38f, 0.53f, 0.87f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.21f, 0.24f, 0.28f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.38f, 0.53f, 0.87f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.38f, 0.53f, 0.87f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.38f, 0.53f, 0.87f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.20f, 0.80f, 0.62f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.20f, 0.80f, 0.62f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.28f, 0.31f, 0.36f, 1.00f);
    } else if (themeName == "Gruvbox") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.16f, 0.15f, 0.13f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.16f, 0.15f, 0.13f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.16f, 0.15f, 0.13f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.40f, 0.36f, 0.32f, 0.50f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.92f, 0.86f, 0.70f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.57f, 0.51f, 0.45f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.24f, 0.22f, 0.20f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.82f, 0.56f, 0.26f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.82f, 0.56f, 0.26f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.16f, 0.15f, 0.13f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.15f, 0.13f, 1.00f);
        style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.16f, 0.15f, 0.13f, 0.51f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.82f, 0.56f, 0.26f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.82f, 0.56f, 0.26f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.72f, 0.73f, 0.15f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.82f, 0.56f, 0.26f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.82f, 0.56f, 0.26f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.82f, 0.56f, 0.26f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.24f, 0.22f, 0.20f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.82f, 0.56f, 0.26f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.82f, 0.56f, 0.26f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.82f, 0.56f, 0.26f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.72f, 0.73f, 0.15f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.72f, 0.73f, 0.15f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.40f, 0.36f, 0.32f, 0.50f);
    } else if (themeName == "Tokyo Night") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.11f, 0.17f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.17f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.11f, 0.17f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.21f, 0.23f, 0.33f, 1.00f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.66f, 0.70f, 0.87f, 1.00f);
        style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.33f, 0.36f, 0.51f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.16f, 0.24f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.48f, 0.52f, 0.98f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.48f, 0.52f, 0.98f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.11f, 0.17f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.11f, 0.17f, 1.00f);
        style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.11f, 0.17f, 0.51f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.48f, 0.52f, 0.98f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.48f, 0.52f, 0.98f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.98f, 0.55f, 0.67f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.48f, 0.52f, 0.98f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.48f, 0.52f, 0.98f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.48f, 0.52f, 0.98f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.16f, 0.24f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.48f, 0.52f, 0.98f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.48f, 0.52f, 0.98f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.48f, 0.52f, 0.98f, 1.00f);
        style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.98f, 0.55f, 0.67f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.45f, 0.89f, 0.85f, 1.00f);
        style.Colors[ImGuiCol_Separator] = ImVec4(0.21f, 0.23f, 0.33f, 1.00f);
    } else if (themeName == "Purple") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.08f, 0.14f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.08f, 0.14f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.08f, 0.14f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.50f, 0.30f, 0.70f, 0.50f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.95f, 0.90f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.15f, 0.28f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.60f, 0.40f, 0.80f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.60f, 0.40f, 0.80f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.08f, 0.14f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.20f, 0.15f, 0.28f, 1.00f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.55f, 0.35f, 0.75f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.65f, 0.45f, 0.85f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.75f, 0.55f, 0.95f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.55f, 0.35f, 0.75f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.65f, 0.45f, 0.85f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.65f, 0.45f, 0.85f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.20f, 0.15f, 0.28f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.65f, 0.45f, 0.85f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.55f, 0.35f, 0.75f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.65f, 0.45f, 0.85f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.80f, 0.60f, 1.00f, 1.00f);
    } else if (themeName == "Pink") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.08f, 0.10f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.08f, 0.10f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.08f, 0.10f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.80f, 0.40f, 0.60f, 0.50f);
        style.Colors[ImGuiCol_Text] = ImVec4(1.00f, 0.92f, 0.96f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.25f, 0.15f, 0.20f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.90f, 0.50f, 0.70f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.90f, 0.50f, 0.70f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.08f, 0.10f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.25f, 0.15f, 0.20f, 1.00f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.85f, 0.45f, 0.65f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.95f, 0.55f, 0.75f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(1.00f, 0.65f, 0.85f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.85f, 0.45f, 0.65f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.95f, 0.55f, 0.75f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.95f, 0.55f, 0.75f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.25f, 0.15f, 0.20f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.95f, 0.55f, 0.75f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.85f, 0.45f, 0.65f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.95f, 0.55f, 0.75f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.70f, 0.90f, 1.00f);
    } else if (themeName == "Blue") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.10f, 0.14f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.30f, 0.50f, 0.80f, 0.50f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.90f, 0.95f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.20f, 0.30f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.40f, 0.60f, 0.90f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.40f, 0.60f, 0.90f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.10f, 0.14f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.15f, 0.20f, 0.30f, 1.00f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.30f, 0.50f, 0.80f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.40f, 0.60f, 0.90f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.50f, 0.70f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.30f, 0.50f, 0.80f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.40f, 0.60f, 0.90f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.40f, 0.60f, 0.90f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.20f, 0.30f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.40f, 0.60f, 0.90f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.30f, 0.50f, 0.80f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.60f, 0.90f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.50f, 0.75f, 1.00f, 1.00f);
    } else if (themeName == "Teal") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.12f, 0.12f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.12f, 0.12f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.12f, 0.12f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.20f, 0.70f, 0.70f, 0.50f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.90f, 1.00f, 1.00f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.22f, 0.22f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.80f, 0.80f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.80f, 0.80f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.12f, 0.12f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.22f, 0.22f, 1.00f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.60f, 0.60f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.80f, 0.80f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.40f, 0.90f, 0.90f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.20f, 0.60f, 0.60f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.80f, 0.80f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.80f, 0.80f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.22f, 0.22f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.80f, 0.80f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.20f, 0.60f, 0.60f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.30f, 0.80f, 0.80f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.40f, 1.00f, 1.00f, 1.00f);
    } else if (themeName == "Red") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.12f, 0.08f, 0.08f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.08f, 0.08f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.08f, 0.08f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.80f, 0.30f, 0.30f, 0.50f);
        style.Colors[ImGuiCol_Text] = ImVec4(1.00f, 0.92f, 0.92f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.25f, 0.12f, 0.12f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.90f, 0.35f, 0.35f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.90f, 0.35f, 0.35f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.12f, 0.08f, 0.08f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.25f, 0.12f, 0.12f, 1.00f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.75f, 0.25f, 0.25f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.90f, 0.35f, 0.35f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(1.00f, 0.45f, 0.45f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.75f, 0.25f, 0.25f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.90f, 0.35f, 0.35f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.90f, 0.35f, 0.35f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.25f, 0.12f, 0.12f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.90f, 0.35f, 0.35f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.75f, 0.25f, 0.25f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.90f, 0.35f, 0.35f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.50f, 0.50f, 1.00f);
    } else if (themeName == "Green") {
        style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.12f, 0.08f, 1.00f);
        style.Colors[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.12f, 0.08f, 1.00f);
        style.Colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.12f, 0.08f, 0.94f);
        style.Colors[ImGuiCol_Border] = ImVec4(0.30f, 0.70f, 0.30f, 0.50f);
        style.Colors[ImGuiCol_Text] = ImVec4(0.92f, 1.00f, 0.92f, 1.00f);
        style.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.22f, 0.12f, 0.54f);
        style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.35f, 0.80f, 0.35f, 0.54f);
        style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.35f, 0.80f, 0.35f, 0.67f);
        style.Colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.12f, 0.08f, 1.00f);
        style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.22f, 0.12f, 1.00f);
        style.Colors[ImGuiCol_Button] = ImVec4(0.25f, 0.60f, 0.25f, 0.40f);
        style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.35f, 0.80f, 0.35f, 1.00f);
        style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.45f, 0.90f, 0.45f, 1.00f);
        style.Colors[ImGuiCol_Header] = ImVec4(0.25f, 0.60f, 0.25f, 0.31f);
        style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.35f, 0.80f, 0.35f, 0.80f);
        style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.35f, 0.80f, 0.35f, 1.00f);
        style.Colors[ImGuiCol_Tab] = ImVec4(0.12f, 0.22f, 0.12f, 0.86f);
        style.Colors[ImGuiCol_TabHovered] = ImVec4(0.35f, 0.80f, 0.35f, 0.80f);
        style.Colors[ImGuiCol_TabSelected] = ImVec4(0.25f, 0.60f, 0.25f, 1.00f);
        style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.80f, 0.35f, 1.00f);
        style.Colors[ImGuiCol_CheckMark] = ImVec4(0.50f, 1.00f, 0.50f, 1.00f);
    }
}

// Apply the saved appearance config (theme and custom colors) to ImGui
void ApplyAppearanceConfig() {
    const std::string& theme = g_config.appearance.theme;

    // Apply base theme - start with Dark as base for all themes
    ImGui::StyleColorsDark();

    // Then apply the specific theme's colors
    if (theme == "Light") {
        ImGui::StyleColorsLight();
    } else if (theme == "Classic") {
        ImGui::StyleColorsClassic();
    } else if (theme == "Dracula" || theme == "Nord" || theme == "Solarized" || theme == "Monokai" || theme == "Catppuccin" ||
               theme == "One Dark" || theme == "Gruvbox" || theme == "Tokyo Night" || theme == "Purple" || theme == "Pink" ||
               theme == "Blue" || theme == "Teal" || theme == "Red" || theme == "Green") {
        ApplyPresetThemeColors(theme);
    }
    // else: stays with Dark theme colors

    // Always set modal window dim color consistently
    ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.5f);

    // Apply custom color overrides if theme is "Custom"
    if (theme == "Custom" && !g_config.appearance.customColors.empty()) {
        ImGuiStyle& style = ImGui::GetStyle();

        // Map color names to ImGui color indices
        static const std::map<std::string, ImGuiCol_> colorNameToIdx = { { "WindowBg", ImGuiCol_WindowBg },
                                                                         { "ChildBg", ImGuiCol_ChildBg },
                                                                         { "PopupBg", ImGuiCol_PopupBg },
                                                                         { "Border", ImGuiCol_Border },
                                                                         { "Text", ImGuiCol_Text },
                                                                         { "TextDisabled", ImGuiCol_TextDisabled },
                                                                         { "FrameBg", ImGuiCol_FrameBg },
                                                                         { "FrameBgHovered", ImGuiCol_FrameBgHovered },
                                                                         { "FrameBgActive", ImGuiCol_FrameBgActive },
                                                                         { "TitleBg", ImGuiCol_TitleBg },
                                                                         { "TitleBgActive", ImGuiCol_TitleBgActive },
                                                                         { "TitleBgCollapsed", ImGuiCol_TitleBgCollapsed },
                                                                         { "Button", ImGuiCol_Button },
                                                                         { "ButtonHovered", ImGuiCol_ButtonHovered },
                                                                         { "ButtonActive", ImGuiCol_ButtonActive },
                                                                         { "Header", ImGuiCol_Header },
                                                                         { "HeaderHovered", ImGuiCol_HeaderHovered },
                                                                         { "HeaderActive", ImGuiCol_HeaderActive },
                                                                         { "Tab", ImGuiCol_Tab },
                                                                         { "TabHovered", ImGuiCol_TabHovered },
                                                                         { "TabSelected", ImGuiCol_TabSelected },
                                                                         { "SliderGrab", ImGuiCol_SliderGrab },
                                                                         { "SliderGrabActive", ImGuiCol_SliderGrabActive },
                                                                         { "ScrollbarBg", ImGuiCol_ScrollbarBg },
                                                                         { "ScrollbarGrab", ImGuiCol_ScrollbarGrab },
                                                                         { "ScrollbarGrabHovered", ImGuiCol_ScrollbarGrabHovered },
                                                                         { "ScrollbarGrabActive", ImGuiCol_ScrollbarGrabActive },
                                                                         { "CheckMark", ImGuiCol_CheckMark },
                                                                         { "TextSelectedBg", ImGuiCol_TextSelectedBg },
                                                                         { "Separator", ImGuiCol_Separator },
                                                                         { "SeparatorHovered", ImGuiCol_SeparatorHovered },
                                                                         { "SeparatorActive", ImGuiCol_SeparatorActive },
                                                                         { "ResizeGrip", ImGuiCol_ResizeGrip },
                                                                         { "ResizeGripHovered", ImGuiCol_ResizeGripHovered },
                                                                         { "ResizeGripActive", ImGuiCol_ResizeGripActive } };

        for (const auto& [name, color] : g_config.appearance.customColors) {
            auto it = colorNameToIdx.find(name);
            if (it != colorNameToIdx.end()) { style.Colors[it->second] = ImVec4(color.r, color.g, color.b, color.a); }
        }
    }

    Log("Applied appearance config: theme=" + theme);
}

// Save theme to separate theme.toml file
void SaveTheme() {
    if (g_toolscreenPath.empty()) {
        Log("ERROR: Cannot save theme, toolscreen path is not available.");
        return;
    }

    std::wstring themePath = g_toolscreenPath + L"\\theme.toml";
    try {
        toml::table tbl;
        tbl.insert_or_assign("theme", g_config.appearance.theme);

        // Persist custom palette alongside the theme name so edits survive restarts
        // even if the main config save is throttled or theme.toml overrides config theme.
        // Always write the table (even if empty) so "Reset" reliably clears saved overrides.
        toml::table colorsTbl;
        for (const auto& [name, color] : g_config.appearance.customColors) {
            colorsTbl.insert(name, ColorToTomlArray(color));
        }
        tbl.insert_or_assign("customColors", colorsTbl);

        std::string narrowPath = WideToUtf8(themePath);
        std::ofstream o(narrowPath);
        if (!o.is_open()) {
            Log("ERROR: Failed to open theme.toml for writing.");
            return;
        }
        o << tbl;
        o.close();
        Log("Saved theme to theme.toml: " + g_config.appearance.theme);
    } catch (const std::exception& e) { Log("ERROR: Failed to save theme: " + std::string(e.what())); }
}

// Load theme from separate theme.toml file
void LoadTheme() {
    if (g_toolscreenPath.empty()) {
        Log("WARNING: Cannot load theme, toolscreen path is not available.");
        return;
    }

    std::wstring themePath = g_toolscreenPath + L"\\theme.toml";
    std::string narrowPath = WideToUtf8(themePath);

    // Check if file exists
    std::ifstream testFile(narrowPath);
    if (!testFile.good()) {
        Log("theme.toml not found, using default theme.");
        return;
    }
    testFile.close();

    try {
        toml::table tbl = toml::parse_file(narrowPath);
        if (tbl.contains("theme")) {
            std::string themeName = tbl["theme"].value_or<std::string>("Dark");
            g_config.appearance.theme = themeName;
            Log("Loaded theme from theme.toml: " + themeName);
        }

        // Optional: load custom palette from theme.toml (newer versions store it here).
        if (const toml::node* ccNode = tbl.get("customColors")) {
            if (const toml::table* colorsTbl = ccNode->as_table()) {
                g_config.appearance.customColors.clear();
                for (const auto& [key, value] : *colorsTbl) {
                    if (auto arr = value.as_array()) {
                        g_config.appearance.customColors[std::string(key.str())] =
                            ColorFromTomlArray(arr, { 0.0f, 0.0f, 0.0f, 1.0f });
                    }
                }
            }
        }
    } catch (const toml::parse_error& e) {
        Log("ERROR: Failed to parse theme.toml: " + std::string(e.what()));
    } catch (const std::exception& e) { Log("ERROR: Failed to load theme: " + std::string(e.what())); }
}

// Helper functions to get default configurations for reset functionality
// These now use the embedded default.toml resource for consistency
std::vector<ModeConfig> GetDefaultModes() { return GetDefaultModesFromEmbedded(); }

std::vector<MirrorConfig> GetDefaultMirrors() { return GetDefaultMirrorsFromEmbedded(); }

std::vector<ImageConfig> GetDefaultImages() { return GetDefaultImagesFromEmbedded(); }

std::vector<WindowOverlayConfig> GetDefaultWindowOverlays() {
    // No default window overlays in embedded config
    return std::vector<WindowOverlayConfig>();
}

std::vector<HotkeyConfig> GetDefaultHotkeys() { return GetDefaultHotkeysFromEmbedded(); }

CursorsConfig GetDefaultCursors() { return GetDefaultCursorsFromEmbedded(); }

void WriteDefaultConfig(const std::wstring& path) {
    int screenWidth = GetCachedScreenWidth();
    int screenHeight = GetCachedScreenHeight();

    // Try to load the embedded default config
    Config defaultConfig;
    if (LoadEmbeddedDefaultConfig(defaultConfig)) {
        // Apply dynamic screen-size adjustments
        for (auto& mode : defaultConfig.modes) {
            if (mode.id == "Fullscreen") {
                mode.width = screenWidth;
                mode.height = screenHeight;
                if (mode.stretch.enabled) {
                    mode.stretch.width = screenWidth;
                    mode.stretch.height = screenHeight;
                }
            } else if (mode.id == "Thin") {
                mode.height = screenHeight;
            } else if (mode.id == "Wide") {
                mode.width = screenWidth;
            }
        }

        // Apply dynamic eyezoom margins
        int horizontalMargin = ((screenWidth / 2) - (384 / 2)) / 10;
        int verticalMargin = (screenHeight / 2) / 4;
        defaultConfig.eyezoom.horizontalMargin = horizontalMargin;
        defaultConfig.eyezoom.verticalMargin = verticalMargin;

        // Apply dynamic Ninjabrain Bot path
        for (auto& image : defaultConfig.images) {
            if (image.name == "Ninjabrain Bot" && image.path.empty()) {
                WCHAR tempPath[MAX_PATH];
                if (GetTempPathW(MAX_PATH, tempPath) > 0) {
                    std::wstring nbImagePath = std::wstring(tempPath) + L"nb-overlay.png";
                    image.path = WideToUtf8(nbImagePath);
                }
            }
        }

        // Apply dynamic cursor size
        HDC hdc = GetDC(NULL);
        int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ReleaseDC(NULL, hdc);
        int systemCursorSize = GetSystemMetricsForDpi(SM_CYCURSOR, dpi);
        if (systemCursorSize < 16) systemCursorSize = 16;
        if (systemCursorSize > 320) systemCursorSize = 320;
        defaultConfig.cursors.title.cursorSize = systemCursorSize;
        defaultConfig.cursors.wall.cursorSize = systemCursorSize;
        defaultConfig.cursors.ingame.cursorSize = systemCursorSize;

        try {
            toml::table tbl;
            ConfigToToml(defaultConfig, tbl);

            std::string narrowPath = WideToUtf8(path);
            std::ofstream o(narrowPath);
            o << tbl;
            o.close();
            Log("Wrote default config.toml from embedded defaults, customized for your monitor (" + std::to_string(screenWidth) + "x" +
                std::to_string(screenHeight) + ").");
        } catch (const std::exception& e) { Log("ERROR: Failed to write default config file: " + std::string(e.what())); }
    } else {
        // Fallback: if embedded config fails, create minimal config
        Log("WARNING: Could not load embedded default config, creating minimal fallback config");
        defaultConfig.configVersion = GetConfigVersion();
        defaultConfig.defaultMode = "Fullscreen";
        defaultConfig.guiHotkey = { VK_CONTROL, 'E' };

        // Create minimal Fullscreen mode
        ModeConfig fullscreenMode;
        fullscreenMode.id = "Fullscreen";
        fullscreenMode.width = screenWidth;
        fullscreenMode.height = screenHeight;
        fullscreenMode.stretch.enabled = true;
        fullscreenMode.stretch.width = screenWidth;
        fullscreenMode.stretch.height = screenHeight;
        defaultConfig.modes.push_back(fullscreenMode);

        try {
            toml::table tbl;
            ConfigToToml(defaultConfig, tbl);

            std::string narrowPath = WideToUtf8(path);
            std::ofstream o(narrowPath);
            o << tbl;
            o.close();
            Log("Wrote fallback config.toml for your monitor (" + std::to_string(screenWidth) + "x" + std::to_string(screenHeight) + ").");
        } catch (const std::exception& e) { Log("ERROR: Failed to write fallback config file: " + std::string(e.what())); }
    }
}

void LoadConfig() {
    PROFILE_SCOPE_CAT("Config Load", "IO Operations");
    if (g_toolscreenPath.empty()) {
        Log("Cannot load config, toolscreen path is not available.");
        return;
    }

    std::wstring configPath = g_toolscreenPath + L"\\config.toml";
    std::string narrowPath = WideToUtf8(configPath);

    // Check if config.toml exists
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

    // Create backup of existing config file
    BackupConfigFile();

    try {
        g_config = Config(); // Initialize with struct defaults
        g_hotkeyTimestamps.clear();

        // Load TOML config
        toml::table tbl = toml::parse_file(narrowPath);
        ConfigFromToml(tbl, g_config);
        Log("Loaded config from TOML file.");

        // Always enforce "Fullscreen" as the default mode, regardless of what's in the config file
        g_config.defaultMode = "Fullscreen";

        int screenWidth = GetCachedScreenWidth();
        int screenHeight = GetCachedScreenHeight();

        // Helper to check if a mode exists
        auto modeExists = [&](const std::string& id) -> bool {
            for (const auto& mode : g_config.modes) {
                if (EqualsIgnoreCase(mode.id, id)) return true;
            }
            return false;
        };

        // Ensure Fullscreen mode exists with current monitor resolution
        if (!modeExists("Fullscreen")) {
            ModeConfig fullscreenMode;
            fullscreenMode.id = "Fullscreen";
            fullscreenMode.width = screenWidth;
            fullscreenMode.height = screenHeight;
            fullscreenMode.stretch.enabled = true;
            fullscreenMode.stretch.x = 0;
            fullscreenMode.stretch.y = 0;
            fullscreenMode.stretch.width = screenWidth;
            fullscreenMode.stretch.height = screenHeight;
            fullscreenMode.mirrorIds.push_back("Mapless");
            g_config.modes.insert(g_config.modes.begin(), fullscreenMode);
            Log("Created missing Fullscreen mode");
        }
        // NOTE: If Fullscreen mode already exists, we preserve its custom resolution.
        // Users can set a custom resolution in the GUI, and it should persist across mode switches.

        // Ensure EyeZoom mode exists
        if (!modeExists("EyeZoom")) {
            ModeConfig eyezoomMode;
            eyezoomMode.id = "EyeZoom";
            eyezoomMode.width = 384;
            eyezoomMode.height = 16384;
            g_config.modes.push_back(eyezoomMode);
            Log("Created missing EyeZoom mode");
        }

        // Ensure Thin mode exists
        if (!modeExists("Thin")) {
            ModeConfig thinMode;
            thinMode.id = "Thin";
            thinMode.width = 300;
            thinMode.height = screenHeight;
            thinMode.background.selectedMode = "color";
            thinMode.background.color = { 45 / 255.0f, 0 / 255.0f, 80 / 255.0f };
            thinMode.mirrorIds.push_back("Mapless");
            g_config.modes.push_back(thinMode);
            Log("Created missing Thin mode");
        }

        // Ensure Wide mode exists
        if (!modeExists("Wide")) {
            ModeConfig wideMode;
            wideMode.id = "Wide";
            wideMode.width = screenWidth;
            wideMode.height = 400;
            wideMode.background.selectedMode = "color";
            wideMode.background.color = { 0.0f, 0.0f, 0.0f };
            wideMode.mirrorIds.push_back("Mapless");
            g_config.modes.push_back(wideMode);
            Log("Created missing Wide mode");
        }

        // Resolve relative sizes to pixel values for all modes
        // This is necessary when loading configs that use percentage-based sizing
        for (auto& mode : g_config.modes) {
            bool widthIsRelative = mode.widthExpr.empty() && mode.relativeWidth >= 0.0f && mode.relativeWidth <= 1.0f;
            bool heightIsRelative = mode.heightExpr.empty() && mode.relativeHeight >= 0.0f && mode.relativeHeight <= 1.0f;

            if (widthIsRelative) {
                mode.width = static_cast<int>(mode.relativeWidth * screenWidth);
                if (mode.width < 1) mode.width = 1;
            }
            if (heightIsRelative) {
                mode.height = static_cast<int>(mode.relativeHeight * screenHeight);
                if (mode.height < 1) mode.height = 1;
            }
        }

        for (auto& hotkey : g_config.hotkeys) {
            if (hotkey.mainMode.empty()) { hotkey.mainMode = g_config.defaultMode; }
        }

        // Initialize thread-safe secondary mode state from loaded config
        ResetAllHotkeySecondaryModes();

        {
            std::lock_guard<std::mutex> lock(g_modeIdMutex);
            if (g_currentModeId.empty()) {
                g_currentModeId = g_config.defaultMode;
                // Update lock-free double-buffer for input handlers
                int nextIndex = 1 - g_currentModeIdIndex.load(std::memory_order_relaxed);
                g_modeIdBuffers[nextIndex] = g_config.defaultMode;
                g_currentModeIdIndex.store(nextIndex, std::memory_order_release);
            }
        }

        Log("Config loaded: " + std::to_string(g_config.modes.size()) + " modes, " + std::to_string(g_config.mirrors.size()) +
            " mirrors, " + std::to_string(g_config.images.size()) + " images, " + std::to_string(g_config.windowOverlays.size()) +
            " window overlays, " + std::to_string(g_config.hotkeys.size()) + " hotkeys.");

        // Check and handle config version upgrades
        int loadedConfigVersion = g_config.configVersion;
        int currentConfigVersion = GetConfigVersion();

        if (loadedConfigVersion < currentConfigVersion) {
            Log("Config version upgrade detected: v" + std::to_string(loadedConfigVersion) + " -> v" +
                std::to_string(currentConfigVersion));

            // ================================================================
            // CONFIG UPGRADE LOGIC
            // ================================================================
            // Add version-specific upgrade logic here as needed.
            // Each upgrade should be idempotent and version-specific.
            //
            // Example for future upgrades:
            // if (loadedConfigVersion < 2) {
            //     // Upgrade from v1 to v2: Add new field with default value
            //     for (auto& mode : g_config.modes) {
            //         if (mode.newFieldFromV2.empty()) {
            //             mode.newFieldFromV2 = "default_value";
            //         }
            //     }
            //     Log("Applied config upgrade: v1 -> v2");
            // }
            // if (loadedConfigVersion < 3) {
            //     // Upgrade from v2 to v3: Rename or restructure fields
            //     // ... upgrade logic here ...
            //     Log("Applied config upgrade: v2 -> v3");
            // }
            // ================================================================

            // Update config version to current
            g_config.configVersion = currentConfigVersion;
            g_configIsDirty = true; // Mark config as dirty to save the updated version
            Log("Config upgraded to version " + std::to_string(currentConfigVersion));
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

        // Rebuild hotkey optimization cache while we still hold the config lock
        // This is safe because we use the internal version that doesn't try to reacquire the lock
        {
            std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
            RebuildHotkeyMainKeys_Internal();
        }

        // Invalidate config lookup caches to force rebuild with new config data
        InvalidateConfigLookupCaches();

        // Set overlay text font size
        SetOverlayTextFontSize(g_config.eyezoom.textFontSize);

        // Evaluate expression-based dimensions with current screen size
        RecalculateExpressionDimensions();

        // Publish initial config snapshot for reader threads (RCU pattern)
        PublishConfigSnapshot();

        // Initialize mirror thread global colorspace mode from loaded config
        SetGlobalMirrorGammaMode(g_config.mirrorGammaMode);

        // Mark config as successfully loaded (must be last line in try block)
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

bool HasDuplicateModeName(const std::string& name, size_t currentIndex) {
    for (size_t i = 0; i < g_config.modes.size(); i++) {
        if (i != currentIndex && g_config.modes[i].id == name) { return true; }
    }
    return false;
}

bool HasDuplicateMirrorName(const std::string& name, size_t currentIndex) {
    for (size_t i = 0; i < g_config.mirrors.size(); i++) {
        if (i != currentIndex && g_config.mirrors[i].name == name) { return true; }
    }
    return false;
}

bool HasDuplicateMirrorGroupName(const std::string& name, size_t currentIndex) {
    for (size_t i = 0; i < g_config.mirrorGroups.size(); i++) {
        if (i != currentIndex && g_config.mirrorGroups[i].name == name) { return true; }
    }
    return false;
}

bool HasDuplicateImageName(const std::string& name, size_t currentIndex) {
    for (size_t i = 0; i < g_config.images.size(); i++) {
        if (i != currentIndex && g_config.images[i].name == name) { return true; }
    }
    return false;
}

bool HasDuplicateWindowOverlayName(const std::string& name, size_t currentIndex) {
    for (size_t i = 0; i < g_config.windowOverlays.size(); i++) {
        if (i != currentIndex && g_config.windowOverlays[i].name == name) { return true; }
    }
    return false;
}

bool Spinner(const char* id_label, int* v, int step, int min_val, int max_val, float inputWidth, float margin) {
    ImGui::PushID(id_label);
    bool value_changed = false;
    float button_size = ImGui::GetFrameHeight();

    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGuiID minus_id = ImGui::GetID("-btn");
    ImGuiID plus_id = ImGui::GetID("+btn");

    // Minus button with repeat
    if (ImGui::Button("-", { button_size, button_size })) {
        *v -= step;
        value_changed = true;
    }
    if (ImGui::IsItemActive()) {
        // Repeat while holding down
        float hold_time = storage->GetFloat(minus_id, 0.0f);
        hold_time += ImGui::GetIO().DeltaTime;
        storage->SetFloat(minus_id, hold_time);

        if (hold_time > spinnerHoldDelay) {
            int repeat_count = (int)((hold_time - spinnerHoldDelay) / spinnerHoldInterval);
            int last_repeat_count = storage->GetInt(ImGui::GetID("-cnt"), 0);
            if (repeat_count > last_repeat_count) {
                *v -= step;
                value_changed = true;
                storage->SetInt(ImGui::GetID("-cnt"), repeat_count);
            }
        }
    } else {
        storage->SetFloat(minus_id, 0.0f);
        storage->SetInt(ImGui::GetID("-cnt"), 0);
    }

    ImGui::SameLine(0, margin);
    ImGui::SetNextItemWidth(inputWidth);
    if (ImGui::InputInt("##value", v, 0, 0)) { value_changed = true; }
    ImGui::SameLine(0, margin);

    // Plus button with repeat
    if (ImGui::Button("+", { button_size, button_size })) {
        *v += step;
        value_changed = true;
    }
    if (ImGui::IsItemActive()) {
        // Repeat while holding down
        float hold_time = storage->GetFloat(plus_id, 0.0f);
        hold_time += ImGui::GetIO().DeltaTime;
        storage->SetFloat(plus_id, hold_time);

        if (hold_time > spinnerHoldDelay) {
            int repeat_count = (int)((hold_time - spinnerHoldDelay) / spinnerHoldInterval);
            int last_repeat_count = storage->GetInt(ImGui::GetID("+cnt"), 0);
            if (repeat_count > last_repeat_count) {
                *v += step;
                value_changed = true;
                storage->SetInt(ImGui::GetID("+cnt"), repeat_count);
            }
        }
    } else {
        storage->SetFloat(plus_id, 0.0f);
        storage->SetInt(ImGui::GetID("+cnt"), 0);
    }

    int clamped_v = *v;
    if (clamped_v < min_val) clamped_v = min_val;
    if (clamped_v > max_val) clamped_v = max_val;
    if (*v != clamped_v) {
        *v = clamped_v;
        value_changed = true;
    }

    ImGui::PopID();
    return value_changed;
}

bool SpinnerFloat(const char* id_label, float* v, float step = 0.1f, float min_val = 0.0f, float max_val = FLT_MAX,
                  const char* format = "%.1f") {
    ImGui::PushID(id_label);
    bool value_changed = false;
    float button_size = ImGui::GetFrameHeight();

    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGuiID minus_id = ImGui::GetID("-btn");
    ImGuiID plus_id = ImGui::GetID("+btn");

    // Minus button with repeat
    if (ImGui::Button("-", { button_size, button_size })) {
        *v -= step;
        value_changed = true;
    }
    if (ImGui::IsItemActive()) {
        // Repeat while holding down
        float hold_time = storage->GetFloat(minus_id, 0.0f);
        hold_time += ImGui::GetIO().DeltaTime;
        storage->SetFloat(minus_id, hold_time);

        if (hold_time > spinnerHoldDelay) {
            int repeat_count = (int)((hold_time - spinnerHoldDelay) / spinnerHoldInterval);
            int last_repeat_count = storage->GetInt(ImGui::GetID("-cnt"), 0);
            if (repeat_count > last_repeat_count) {
                *v -= step;
                value_changed = true;
                storage->SetInt(ImGui::GetID("-cnt"), repeat_count);
            }
        }
    } else {
        storage->SetFloat(minus_id, 0.0f);
        storage->SetInt(ImGui::GetID("-cnt"), 0);
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputFloat("##value", v, 0.0f, 0.0f, format)) { value_changed = true; }
    ImGui::SameLine();

    // Plus button with repeat
    if (ImGui::Button("+", { button_size, button_size })) {
        *v += step;
        value_changed = true;
    }
    if (ImGui::IsItemActive()) {
        // Repeat while holding down
        float hold_time = storage->GetFloat(plus_id, 0.0f);
        hold_time += ImGui::GetIO().DeltaTime;
        storage->SetFloat(plus_id, hold_time);

        if (hold_time > spinnerHoldDelay) {
            int repeat_count = (int)((hold_time - spinnerHoldDelay) / spinnerHoldInterval);
            int last_repeat_count = storage->GetInt(ImGui::GetID("+cnt"), 0);
            if (repeat_count > last_repeat_count) {
                *v += step;
                value_changed = true;
                storage->SetInt(ImGui::GetID("+cnt"), repeat_count);
            }
        }
    } else {
        storage->SetFloat(plus_id, 0.0f);
        storage->SetInt(ImGui::GetID("+cnt"), 0);
    }

    float clamped_v = *v;
    if (clamped_v < min_val) clamped_v = min_val;
    if (clamped_v > max_val) clamped_v = max_val;
    if (*v != clamped_v) {
        *v = clamped_v;
        value_changed = true;
    }

    ImGui::PopID();
    return value_changed;
}

void RenderConfigErrorGUI() {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 center = ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(600, 0));
    if (ImGui::Begin("Configuration Error", NULL,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove)) {
        static std::chrono::steady_clock::time_point s_lastCopyTime{};
        std::string errorMsg;
        {
            std::lock_guard<std::mutex> lock(g_configErrorMutex);
            errorMsg = g_configLoadError;
        }
        ImGui::TextWrapped("A critical error occurred while loading the configuration file (config.toml).");
        ImGui::Separator();
        ImGui::TextWrapped("%s", errorMsg.c_str());
        ImGui::Separator();
        ImGui::TextWrapped("The application cannot continue. To get help, copy the debug info and send it to a "
                           "developer. Otherwise, please quit the game.");
        ImGui::Separator();

        bool show_feedback =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - s_lastCopyTime).count() < 3;

        // Center the buttons
        float button_width_copy = ImGui::CalcTextSize("Copy Debug Info").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float button_width_quit = ImGui::CalcTextSize("Quit").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        float total_button_width = button_width_copy + button_width_quit + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - total_button_width) * 0.5f);

        if (ImGui::Button("Copy Debug Info")) {
            std::string configContent = "ERROR: Could not read config.toml.";
            std::wstring configPath = g_toolscreenPath + L"\\config.toml";
            std::ifstream f(configPath);
            if (f.is_open()) {
                std::ostringstream ss;
                ss << f.rdbuf();
                configContent = ss.str();
                f.close();
            }

            std::string fullDebugInfo = "Error Message:\r\n";
            fullDebugInfo += "----------------------------------------\r\n";
            fullDebugInfo += errorMsg;
            fullDebugInfo += "\r\n\r\n\r\nRaw config.toml Content:\r\n";
            fullDebugInfo += "----------------------------------------\r\n";
            fullDebugInfo += configContent;

            CopyToClipboard(g_minecraftHwnd.load(), fullDebugInfo);
            s_lastCopyTime = std::chrono::steady_clock::now();
        }

        ImGui::SameLine();
        if (ImGui::Button("Quit")) { exit(0); }

        if (show_feedback) {
            const char* feedback_text = "Debug info copied to clipboard!";
            float feedback_width = ImGui::CalcTextSize(feedback_text).x;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - feedback_width) * 0.5f);
            ImGui::TextUnformatted(feedback_text);
        }

        ImGui::End();
    }
}

struct ExclusionBindState {
    int hotkey_idx = -1;
    int exclusion_idx = -1;
};

struct AltBindState {
    int hotkey_idx = -1;
    int alt_idx = -1;
};

static int s_mainHotkeyToBind = -1;
static int s_sensHotkeyToBind = -1; // Sensitivity hotkey binding state
static ExclusionBindState s_exclusionToBind = { -1, -1 };
static AltBindState s_altHotkeyToBind = { -1, -1 };
static std::atomic<bool> s_isRebindBindingActive{ false };

bool IsHotkeyBindingActive() {
    return s_mainHotkeyToBind != -1 || s_sensHotkeyToBind != -1 || s_exclusionToBind.hotkey_idx != -1 || s_altHotkeyToBind.hotkey_idx != -1;
}

bool IsRebindBindingActive() { return s_isRebindBindingActive.load(std::memory_order_relaxed); }

void ResetTransientBindingUiState() { s_isRebindBindingActive.store(false, std::memory_order_relaxed); }

void MarkRebindBindingActive() { s_isRebindBindingActive.store(true, std::memory_order_relaxed); }

void RenderSettingsGUI() {
    PROFILE_SCOPE_CAT("Settings GUI Rendering", "ImGui");
    ResetTransientBindingUiState();

    static const std::vector<std::pair<const char*, const char*>> relativeToOptions = { { "topLeftViewport", "Top Left (Viewport)" },
                                                                                        { "topRightViewport", "Top Right (Viewport)" },
                                                                                        { "bottomLeftViewport", "Bottom Left (Viewport)" },
                                                                                        { "bottomRightViewport",
                                                                                          "Bottom Right (Viewport)" },
                                                                                        { "centerViewport", "Center (Viewport)" },
                                                                                        { "pieLeft", "Pie-Chart Left" },
                                                                                        { "pieRight", "Pie-Chart Right" },
                                                                                        { "topLeftScreen", "Top Left (Screen)" },
                                                                                        { "topRightScreen", "Top Right (Screen)" },
                                                                                        { "bottomLeftScreen", "Bottom Left (Screen)" },
                                                                                        { "bottomRightScreen", "Bottom Right (Screen)" },
                                                                                        { "centerScreen", "Center (Screen)" } };
    static const std::vector<std::pair<const char*, const char*>> imageRelativeToOptions = {
        { "topLeftViewport", "Top Left (Viewport)" },       { "topRightViewport", "Top Right (Viewport)" },
        { "bottomLeftViewport", "Bottom Left (Viewport)" }, { "bottomRightViewport", "Bottom Right (Viewport)" },
        { "centerViewport", "Center (Viewport)" },          { "topLeftScreen", "Top Left (Screen)" },
        { "topRightScreen", "Top Right (Screen)" },         { "bottomLeftScreen", "Bottom Left (Screen)" },
        { "bottomRightScreen", "Bottom Right (Screen)" },   { "centerScreen", "Center (Screen)" }
    };
    auto getFriendlyName = [&](const std::string& key, const std::vector<std::pair<const char*, const char*>>& options) {
        for (const auto& option : options) {
            if (key == option.first) return option.second;
        }
        return "Unknown";
    };

    // Static state for tracking keys during binding
    static std::vector<DWORD> s_bindingKeys;
    static bool s_hadKeysPressed = false;
    static std::set<DWORD> s_preHeldKeys;
    static bool s_bindingInitialized = false;

    static const std::vector<const char*> validGameStates = { "wall",    "inworld,cursor_free", "inworld,cursor_grabbed", "title",
                                                              "waiting", "generating" };

    // GUI display states (subset of validGameStates, with "waiting" and "generating" combined)
    static const std::vector<const char*> guiGameStates = { "wall", "inworld,cursor_free", "inworld,cursor_grabbed", "title",
                                                            "generating" };

    // User-friendly names for game states
    static const std::vector<std::pair<const char*, const char*>> gameStateDisplayNames = {
        { "wall", "Wall Screen" },
        { "inworld,cursor_free", "In World (Cursor Free)" },
        { "inworld,cursor_grabbed", "In World (Cursor Grabbed)" },
        { "title", "Title Screen" },
        { "waiting", "Waiting Screen" },
        { "generating", "World Generation" }
    };

    auto getGameStateFriendlyName = [&](const std::string& gameState) {
        for (const auto& pair : gameStateDisplayNames) {
            if (gameState == pair.first) return pair.second;
        }
        return gameState.c_str(); // Fallback to original name
    };

    bool is_binding = IsHotkeyBindingActive();

    if (is_binding) {
        if (!s_bindingInitialized) {
            s_preHeldKeys.clear();
            for (int vk = 1; vk < 0xFF; ++vk) {
                if (GetAsyncKeyState(vk) & 0x8000) {
                    s_preHeldKeys.insert(static_cast<DWORD>(vk));
                }
            }
            s_bindingInitialized = true;
        }
        ImGui::OpenPopup("Bind Hotkey");
    } else {
        s_bindingKeys.clear();
        s_hadKeysPressed = false;
        s_preHeldKeys.clear();
        s_bindingInitialized = false;
    }

    if (ImGui::BeginPopupModal("Bind Hotkey", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::Text("Press a key or key combination.");
        ImGui::Text("Release all keys to confirm.");
        ImGui::Text("Press ESC to cancel.");
        ImGui::Separator();

        // Check for escape to cancel
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            Log("Binding cancelled from Escape key.");
            s_mainHotkeyToBind = -1;
            s_sensHotkeyToBind = -1;
            s_exclusionToBind = { -1, -1 };
            s_altHotkeyToBind = { -1, -1 };
            s_bindingKeys.clear();
            s_hadKeysPressed = false;
            s_preHeldKeys.clear();
            s_bindingInitialized = false;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }

        // Helper to finalize the binding
        auto finalize_bind = [&](const std::vector<DWORD>& keys) {
            if (keys.empty()) return;

            if (s_mainHotkeyToBind != -1) {
                if (s_mainHotkeyToBind == -999) {
                    // Special case for GUI hotkey
                    g_config.guiHotkey = keys;
                } else {
                    g_config.hotkeys[s_mainHotkeyToBind].keys = keys;
                }
                s_mainHotkeyToBind = -1;
            } else if (s_sensHotkeyToBind != -1) {
                // Sensitivity hotkey binding
                g_config.sensitivityHotkeys[s_sensHotkeyToBind].keys = keys;
                s_sensHotkeyToBind = -1;
            } else if (s_altHotkeyToBind.hotkey_idx != -1) {
                g_config.hotkeys[s_altHotkeyToBind.hotkey_idx].altSecondaryModes[s_altHotkeyToBind.alt_idx].keys = keys;
                s_altHotkeyToBind = { -1, -1 };
            } else if (s_exclusionToBind.hotkey_idx != -1) {
                // For exclusions, only use the last (main) key
                g_config.hotkeys[s_exclusionToBind.hotkey_idx].conditions.exclusions[s_exclusionToBind.exclusion_idx] = keys.back();
                s_exclusionToBind = { -1, -1 };
            }
            g_configIsDirty = true;

            // Rebuild hotkey cache
            {
                std::lock_guard<std::mutex> hotkeyLock(g_hotkeyMainKeysMutex);
                RebuildHotkeyMainKeys_Internal();
            }

            s_bindingKeys.clear();
            s_hadKeysPressed = false;
            s_preHeldKeys.clear();
            s_bindingInitialized = false;
            ImGui::CloseCurrentPopup();
        };

        // Evict pre-held keys once they are physically released
        for (auto it = s_preHeldKeys.begin(); it != s_preHeldKeys.end(); ) {
            if (!(GetAsyncKeyState(*it) & 0x8000)) {
                it = s_preHeldKeys.erase(it);
            } else {
                ++it;
            }
        }

        // Build list of currently pressed keys (excluding pre-held keys)
        std::vector<DWORD> currentlyPressed;

        const bool lctrlDown = (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0;
        const bool rctrlDown = (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0;
        const bool lshiftDown = (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0;
        const bool rshiftDown = (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0;
        const bool laltDown = (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0;
        const bool raltDown = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;

        const bool ctrlPreHeld = s_preHeldKeys.count(VK_LCONTROL) || s_preHeldKeys.count(VK_RCONTROL) || s_preHeldKeys.count(VK_CONTROL);
        const bool shiftPreHeld = s_preHeldKeys.count(VK_LSHIFT) || s_preHeldKeys.count(VK_RSHIFT) || s_preHeldKeys.count(VK_SHIFT);
        const bool altPreHeld = s_preHeldKeys.count(VK_LMENU) || s_preHeldKeys.count(VK_RMENU) || s_preHeldKeys.count(VK_MENU);

        if ((lctrlDown || rctrlDown) && !ctrlPreHeld) currentlyPressed.push_back(VK_CONTROL);
        if ((lshiftDown || rshiftDown) && !shiftPreHeld) currentlyPressed.push_back(VK_SHIFT);
        if ((laltDown || raltDown) && !altPreHeld) currentlyPressed.push_back(VK_MENU);

        // Check all other keys
        for (int vk = 1; vk < 0xFF; ++vk) {
            // Skip escape (used for cancel), generic modifiers, and Windows keys
            if (vk == VK_ESCAPE || vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
                vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LMENU || vk == VK_RMENU) {
                continue;
            }
            if (s_preHeldKeys.count(static_cast<DWORD>(vk))) continue;
            if (GetAsyncKeyState(vk) & 0x8000) { currentlyPressed.push_back(vk); }
        }

        // Add any newly pressed keys to our binding (accumulate, don't replace)
        // This allows the user to press keys sequentially (e.g., Alt then B)
        for (DWORD key : currentlyPressed) {
            if (std::find(s_bindingKeys.begin(), s_bindingKeys.end(), key) == s_bindingKeys.end()) {
                // New key - add it in the right position
                // Modifiers should be at the front, main key at the end
                bool isModifier = (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU || key == VK_LCONTROL || key == VK_RCONTROL ||
                                   key == VK_LSHIFT || key == VK_RSHIFT || key == VK_LMENU || key == VK_RMENU);
                if (isModifier) {
                    // Insert modifiers before non-modifiers
                    auto insertPos = s_bindingKeys.begin();
                    for (auto it = s_bindingKeys.begin(); it != s_bindingKeys.end(); ++it) {
                        bool itIsModifier = (*it == VK_CONTROL || *it == VK_SHIFT || *it == VK_MENU || *it == VK_LCONTROL || *it == VK_RCONTROL ||
                                             *it == VK_LSHIFT || *it == VK_RSHIFT || *it == VK_LMENU || *it == VK_RMENU);
                        if (!itIsModifier) {
                            insertPos = it;
                            break;
                        }
                        insertPos = it + 1;
                    }
                    s_bindingKeys.insert(insertPos, key);
                } else {
                    // Non-modifiers go at the end
                    s_bindingKeys.push_back(key);
                }
            }
        }

        if (!currentlyPressed.empty()) { s_hadKeysPressed = true; }

        // If we had keys pressed and now all are released, finalize the binding
        if (s_hadKeysPressed && currentlyPressed.empty()) {
            finalize_bind(s_bindingKeys);
            ImGui::EndPopup();
            return;
        }

        // Display current keys being held
        if (!s_bindingKeys.empty()) {
            std::string combo = GetKeyComboString(s_bindingKeys);
            ImGui::Text("Current: %s", combo.c_str());
        } else {
            ImGui::Text("Current: [None]");
        }

        ImGui::EndPopup();
    }

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSizeConstraints(ImVec2(500, 400), ImVec2(FLT_MAX, FLT_MAX));

    const int screenWidth = GetCachedScreenWidth();
    const int screenHeight = GetCachedScreenHeight();
    float scaleFactor = 1.0f;
    if (screenHeight > 1080) { scaleFactor = static_cast<float>(screenHeight) / 1080.0f; }
    scaleFactor = roundf(scaleFactor * 4.0f) / 4.0f;
    if (scaleFactor < 1.0f) { scaleFactor = 1.0f; }

    if (g_guiNeedsRecenter.exchange(false)) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(850 * scaleFactor, 650 * scaleFactor), ImGuiCond_Always);
    }

    // Create window title with version info
    std::string windowTitle = "Toolscreen v" + GetToolscreenVersionString() + " by jojoe77777";

    // Use a bool for the close button in the title bar
    bool windowOpen = true;
    if (ImGui::Begin(windowTitle.c_str(), &windowOpen, ImGuiWindowFlags_NoCollapse)) {
        // Handle close button click from title bar
        if (!windowOpen) {
            g_showGui = false;
            if (!g_wasCursorVisible.load()) {
                RECT fullScreenRect;
                fullScreenRect.left = 0;
                fullScreenRect.top = 0;
                fullScreenRect.right = GetCachedScreenWidth();
                fullScreenRect.bottom = GetCachedScreenHeight();
                ClipCursor(&fullScreenRect);
                SetCursor(NULL);
            }
            g_currentlyEditingMirror = "";
            g_imageDragMode.store(false);
            g_windowOverlayDragMode.store(false);
            extern std::string s_hoveredImageName;
            extern std::string s_draggedImageName;
            extern bool s_isDragging;
            s_hoveredImageName = "";
            s_draggedImageName = "";
            s_isDragging = false;
            extern std::string s_hoveredWindowOverlayName;
            extern std::string s_draggedWindowOverlayName;
            extern bool s_isWindowOverlayDragging;
            s_hoveredWindowOverlayName = "";
            s_draggedWindowOverlayName = "";
            s_isWindowOverlayDragging = false;
        }

        // Screenshot button at top right (before everything else, so it's always in the same spot)
        {
            static std::chrono::steady_clock::time_point s_lastScreenshotTime{};
            auto now = std::chrono::steady_clock::now();
            bool showCopied = std::chrono::duration_cast<std::chrono::seconds>(now - s_lastScreenshotTime).count() < 3;

            const char* buttonLabel = showCopied ? "Copied!" : "Screenshot";
            float buttonWidth = ImGui::CalcTextSize(buttonLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f;

            // Save cursor position to restore after
            ImVec2 savedCursor = ImGui::GetCursorPos();

            // Position at top right, accounting for close button width (~25px) and some padding
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - buttonWidth - ImGui::GetStyle().WindowPadding.x, 30.0f));

            if (ImGui::Button(buttonLabel)) {
                g_screenshotRequested = true;
                s_lastScreenshotTime = std::chrono::steady_clock::now();
            }

            // Restore cursor position
            ImGui::SetCursorPos(savedCursor);
        }

        // --- BASIC/ADVANCED MODE TOGGLE ---
        {
            bool isAdvanced = !g_config.basicModeEnabled;
            if (ImGui::RadioButton("Basic", !isAdvanced)) {
                g_config.basicModeEnabled = true;
                g_configIsDirty = true;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Advanced", isAdvanced)) {
                g_config.basicModeEnabled = false;
                g_configIsDirty = true;
            }
        }

        ImGui::Separator();

        if (g_config.basicModeEnabled) {
            // --- BASIC MODE: Only General and Other tabs ---
            if (ImGui::BeginTabBar("BasicSettingsTabs")) {
                // =====================================================================
                // BASIC GENERAL TAB - Simplified mode selection with inline hotkeys
                // =====================================================================
#include "gui/tab_basic_general.inl"
                // =====================================================================
                // BASIC OTHER TAB - Miscellaneous settings
                // =====================================================================
#include "gui/tab_basic_other.inl"

                ImGui::EndTabBar();
            }
        } else {
            // --- ADVANCED MODE: All tabs ---
            if (ImGui::BeginTabBar("SettingsTabs")) {
                // =====================================================================
                // MODES TAB - Extracted to gui/tab_modes.inl
                // =====================================================================
#include "gui/tab_modes.inl"
                // =====================================================================
                // MIRRORS TAB - Extracted to gui/tab_mirrors.inl
                // =====================================================================
#include "gui/tab_mirrors.inl"
                // =====================================================================
                // IMAGES TAB - Extracted to gui/tab_images.inl
                // =====================================================================
#include "gui/tab_images.inl"
                // =====================================================================
                // WINDOW OVERLAYS TAB - Extracted to gui/tab_window_overlays.inl
                // =====================================================================
#include "gui/tab_window_overlays.inl"
                // =====================================================================
                // HOTKEYS TAB - Extracted to gui/tab_hotkeys.inl
                // =====================================================================
#include "gui/tab_hotkeys.inl"
                // =====================================================================
                // INPUTS TAB - Contains Mouse and Keyboard sub-tabs
                // =====================================================================
#include "gui/tab_inputs.inl"
                // =====================================================================
                // SETTINGS TAB - Extracted to gui/tab_settings.inl
                // =====================================================================
#include "gui/tab_settings.inl"

                // =====================================================================
                // APPEARANCE TAB - GUI color scheme configuration
                // =====================================================================
#include "gui/tab_appearance.inl"

                // =====================================================================
                // MISC TAB - Extracted to gui/tab_misc.inl
                // =====================================================================
#include "gui/tab_misc.inl"

                ImGui::EndTabBar();
            }
        }

    } else {
        g_currentlyEditingMirror = "";
    }
    ImGui::End();

    // Periodic save while GUI is open (throttled to 1 second)
    SaveConfig();

    // Ensure config snapshot is published for reader threads after GUI mutations.
    // SaveConfig already publishes when it runs, but it's throttled to 1s.
    // For structural changes (push_back/erase on vectors) we need immediate snapshot
    // update to prevent reader threads from seeing stale/freed vector data.
    if (g_configIsDirty.load()) { PublishConfigSnapshot(); }
}

void HandleImGuiContextReset() {
    if (ImGui::GetCurrentContext()) {
        Log("Performing deferred full ImGui context reset.");
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
}

void InitializeImGuiContext(HWND hwnd) {
    if (ImGui::GetCurrentContext() == nullptr) {
        Log("Re-creating ImGui context after full reset.");
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        (void)io;

        const int screenHeight = GetCachedScreenHeight();
        float scaleFactor = 1.0f;
        if (screenHeight > 1080) { scaleFactor = static_cast<float>(screenHeight) / 1080.0f; }
        scaleFactor = roundf(scaleFactor * 4.0f) / 4.0f;
        if (scaleFactor < 1.0f) { scaleFactor = 1.0f; }

        std::string fontPath = g_config.fontPath;
        const float baseFontSize = 16.0f * scaleFactor;

        // Some font files (or paths) can cause ImGui font loading/build to fail.
        // If that happens, ignore the custom font and fall back to Arial.
        auto isStable = [](const std::string& p, float sz) -> bool {
            if (p.empty()) return false;
            ImFontAtlas testAtlas;
            ImFont* f = testAtlas.AddFontFromFileTTF(p.c_str(), sz);
            if (!f) return false;
            return testAtlas.Build();
        };

        std::string usePath = fontPath.empty() ? ConfigDefaults::CONFIG_FONT_PATH : fontPath;
        if (!isStable(usePath, baseFontSize)) { usePath = ConfigDefaults::CONFIG_FONT_PATH; }

        ImFont* baseFont = io.Fonts->AddFontFromFileTTF(usePath.c_str(), baseFontSize);
        if (!baseFont && usePath != ConfigDefaults::CONFIG_FONT_PATH) {
            baseFont = io.Fonts->AddFontFromFileTTF(ConfigDefaults::CONFIG_FONT_PATH.c_str(), baseFontSize);
        }
        if (!baseFont) {
            Log("GUI: Failed to load configured font, using ImGui default font");
            io.Fonts->AddFontDefault();
        }

        ImGui::StyleColorsDark();
        LoadTheme();             // Load theme from theme.toml
        ApplyAppearanceConfig(); // Apply saved theme
        ImGui::GetStyle().ScaleAllSizes(scaleFactor);

        ImGui_ImplWin32_Init(hwnd);
        ImGui_ImplOpenGL3_Init("#version 330");

        // Initialize larger font for overlay text labels
        InitializeOverlayTextFont(usePath, 16.0f, scaleFactor);
    }
}

bool IsGuiHotkeyPressed(WPARAM wParam) { return CheckHotkeyMatch(g_config.guiHotkey, wParam); }

// Welcome toast state
std::atomic<bool> g_welcomeToastVisible{ false };
std::atomic<bool> g_configurePromptDismissedThisSession{ false };

void RenderWelcomeToast(bool isFullscreen) {
    // Semantics:
    // - toast1 (windowed fullscreenPrompt) should ALWAYS show in windowed mode.
    // - toast2 (fullscreen configurePrompt) should ALWAYS show in fullscreen UNTIL Ctrl+I is pressed for this session.
    if (isFullscreen && g_configurePromptDismissedThisSession.load(std::memory_order_relaxed)) { return; }

    // Core-profile-safe rendering (Minecraft 1.17+): use shaders + VAO/VBO.
    static GLuint s_program = 0;
    static GLuint s_vao = 0;
    static GLuint s_vbo = 0;
    static GLint s_locTexture = -1;
    static GLint s_locOpacity = -1;

    static GLuint s_toast1Texture = 0;
    static GLuint s_toast2Texture = 0;
    static int s_toast1Width = 0, s_toast1Height = 0;
    static int s_toast2Width = 0, s_toast2Height = 0;

    // Reset GL objects when context changes.
    static HGLRC s_lastCtx = NULL;
    HGLRC currentCtx = wglGetCurrentContext();
    if (currentCtx != s_lastCtx) {
        s_lastCtx = currentCtx;
        s_program = 0;
        s_vao = 0;
        s_vbo = 0;
        s_locTexture = -1;
        s_locOpacity = -1;
        s_toast1Texture = 0;
        s_toast2Texture = 0;
        s_toast1Width = s_toast1Height = 0;
        s_toast2Width = s_toast2Height = 0;
    }

    // Ensure shader program exists
    if (s_program == 0) {
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

        s_program = CreateShaderProgram(vtxSrc, fragSrc);
        if (s_program != 0) {
            s_locTexture = glGetUniformLocation(s_program, "uTexture");
            s_locOpacity = glGetUniformLocation(s_program, "uOpacity");
            glUseProgram(s_program);
            glUniform1i(s_locTexture, 0);
            glUseProgram(0);
        }
    }

    // Ensure VAO/VBO exist
    if (s_vao == 0) {
        glGenVertexArrays(1, &s_vao);
    }
    if (s_vbo == 0) {
        glGenBuffers(1, &s_vbo);
    }
    if (s_vao != 0 && s_vbo != 0) {
        glBindVertexArray(s_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glBufferData(GL_ARRAY_BUFFER, 6 * 4 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }

    // Ensure textures exist (lazy load from embedded resources)
    auto ensureToastTexture = [&](int resourceId, GLuint& outTexture, int& outW, int& outH) {
        if (outTexture != 0 && outW > 0 && outH > 0) { return; }

        // Disable vertical flip for toast textures.
        stbi_set_flip_vertically_on_load_thread(0);

        HMODULE hModule = NULL;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&RenderWelcomeToast, &hModule);
        if (!hModule) { return; }

        HRSRC hResource = FindResourceW(hModule, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
        if (!hResource) { return; }
        HGLOBAL hData = LoadResource(hModule, hResource);
        if (!hData) { return; }

        DWORD dataSize = SizeofResource(hModule, hResource);
        const unsigned char* rawData = (const unsigned char*)LockResource(hData);
        if (!rawData || dataSize == 0) { return; }

        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = stbi_load_from_memory(rawData, (int)dataSize, &w, &h, &channels, 4);
        if (!pixels || w <= 0 || h <= 0) { return; }

        glGenTextures(1, &outTexture);
        glBindTexture(GL_TEXTURE_2D, outTexture);
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

        outW = w;
        outH = h;
        stbi_image_free(pixels);
    };

    ensureToastTexture(IDR_TOAST1_PNG, s_toast1Texture, s_toast1Width, s_toast1Height);
    ensureToastTexture(IDR_TOAST2_PNG, s_toast2Texture, s_toast2Width, s_toast2Height);

    // Pick texture based on fullscreen state
    GLuint texture = isFullscreen ? s_toast2Texture : s_toast1Texture;
    int imgW = isFullscreen ? s_toast2Width : s_toast1Width;
    int imgH = isFullscreen ? s_toast2Height : s_toast1Height;
    if (s_program == 0 || s_vao == 0 || s_vbo == 0 || texture == 0 || imgW <= 0 || imgH <= 0) { return; }

    // Viewport size
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    int vpW = viewport[2];
    int vpH = viewport[3];
    if (vpW <= 0 || vpH <= 0) { return; }

    // Save GL state (minimal but robust)
    GLint savedProgram = 0, savedVAO = 0, savedVBO = 0, savedFBO = 0, savedTex = 0, savedActiveTex = 0;
    GLboolean savedBlend = GL_FALSE, savedDepthTest = GL_FALSE, savedScissor = GL_FALSE, savedStencil = GL_FALSE;
    GLint savedBlendSrcRGB = 0, savedBlendDstRGB = 0, savedBlendSrcA = 0, savedBlendDstA = 0;
    GLint savedViewport[4];
    GLboolean savedColorMask[4];
    GLint savedUnpackRowLength = 0, savedUnpackSkipPixels = 0, savedUnpackSkipRows = 0, savedUnpackAlignment = 0;

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
    // IMPORTANT: Do NOT force framebuffer 0 here.
    // The render thread draws overlays into an offscreen FBO and then blits it; binding 0 would
    // render the toast into the default framebuffer and it would never show up in the final output.
    // Also avoid stomping the caller's viewport; use the currently-active viewport we queried above.
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    // Scale based on viewport (baseline 1080p)
    float scaleFactor = (static_cast<float>(vpH) / 1080.0f) * 0.45f;
    float drawW = (float)imgW * scaleFactor;
    float drawH = (float)imgH * scaleFactor;

    // Top-left placement in NDC
    float px1 = 0.0f, py1 = 0.0f;
    float px2 = drawW, py2 = drawH;
    float nx1 = (px1 / vpW) * 2.0f - 1.0f;
    float nx2 = (px2 / vpW) * 2.0f - 1.0f;
    float ny_top = 1.0f - (py1 / vpH) * 2.0f;
    float ny_bot = 1.0f - (py2 / vpH) * 2.0f;

    float verts[] = {
        nx1, ny_bot, 0.0f, 1.0f,
        nx2, ny_bot, 1.0f, 1.0f,
        nx2, ny_top, 1.0f, 0.0f,
        nx1, ny_bot, 0.0f, 1.0f,
        nx2, ny_top, 1.0f, 0.0f,
        nx1, ny_top, 0.0f, 0.0f,
    };

    // Draw
    glUseProgram(s_program);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1f(s_locOpacity, 1.0f);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // Restore state
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

void RenderPerformanceOverlay(bool showPerformanceOverlay) {
    if (!showPerformanceOverlay) return;

    static auto lastOverlayUpdate = std::chrono::steady_clock::now();
    static float cachedFrameTime = 0.0f;
    static float cachedOriginalFrameTime = 0.0f;

    auto currentTime = std::chrono::steady_clock::now();
    auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastOverlayUpdate);

    if (timeSinceLastUpdate.count() >= 500) {
        cachedFrameTime = static_cast<float>(g_lastFrameTimeMs.load());
        cachedOriginalFrameTime = static_cast<float>(g_originalFrameTimeMs.load());
        lastOverlayUpdate = currentTime;
    }

    ImGui::SetNextWindowPos(ImVec2(5.0f, 5.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("DebugOverlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Render Hook Overhead: %.2f ms", cachedFrameTime);
    ImGui::Text("Original Frame Time: %.2f ms", cachedOriginalFrameTime);
    ImGui::End();
}

void RenderProfilerOverlay(bool showProfiler, bool showPerformanceOverlay) {
    if (!showProfiler) return;

    auto displayData = Profiler::GetInstance().GetProfileData();

    ImGui::SetNextWindowPos(ImVec2(5.0f, showPerformanceOverlay ? 80.0f : 5.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("ProfilerOverlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_AlwaysAutoResize);

    // Apply configured scale
    ImGui::SetWindowFontScale(g_config.debug.profilerScale);

    ImGui::Text("Toolscreen Profiler (Hierarchical)");
    ImGui::Separator();

    // Helper lambda to render a tree section
    auto renderTreeSection = [](const char* sectionTitle, const std::vector<std::pair<std::string, Profiler::ProfileEntry>>& entries,
                                ImVec4 headerColor) {
        if (entries.empty()) return;

        // Section header
        ImGui::PushStyleColor(ImGuiCol_Text, headerColor);
        ImGui::Text("%s", sectionTitle);
        ImGui::PopStyleColor();

        if (ImGui::BeginTable("##ProfilerTable", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoHostExtendX)) {
            ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthFixed, 280.0f);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Self", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Of Parent", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("Of Total", ImGuiTableColumnFlags_WidthFixed, 60.0f);

            for (size_t i = 0; i < entries.size(); ++i) {
                const auto& [name, entry] = entries[i];

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);

                // Build indentation string with tree characters
                std::string indent;
                for (int d = 0; d < entry.depth; ++d) {
                    indent += "  "; // 2 spaces per depth level
                }

                // Determine if this is the last child at this depth
                bool isLastAtDepth = true;
                for (size_t j = i + 1; j < entries.size(); ++j) {
                    if (entries[j].second.depth == entry.depth) {
                        isLastAtDepth = false;
                        break;
                    } else if (entries[j].second.depth < entry.depth) {
                        break;
                    }
                }

                // Add tree connector
                if (entry.depth > 0) {
                    if (isLastAtDepth) {
                        indent += "â””â”€ ";
                    } else {
                        indent += "â”œâ”€ ";
                    }
                }

                // Color based on depth (special gray for Unspecified entries)
                bool isUnspecified = (name == "[Unspecified]");
                if (isUnspecified) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.6f, 0.6f, 1.0f)); // Dimmer gray for unspecified
                } else if (entry.depth == 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f)); // Yellow for root
                } else if (entry.depth == 1) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.9f, 1.0f, 1.0f)); // Light blue
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f)); // Gray
                }

                ImGui::Text("%s%s", indent.c_str(), name.c_str());
                ImGui::PopStyleColor();

                ImGui::TableSetColumnIndex(1);
                if (entry.rollingAverageTime >= 0.0001) {
                    ImGui::Text("%.4fms", entry.rollingAverageTime);
                } else {
                    ImGui::Text("<0.0001");
                }

                ImGui::TableSetColumnIndex(2);
                if (entry.rollingSelfTime >= 0.0001) {
                    ImGui::Text("%.4fms", entry.rollingSelfTime);
                } else {
                    ImGui::Text("<0.0001");
                }

                ImGui::TableSetColumnIndex(3);
                if (entry.parentPercentage >= 1.0) {
                    ImGui::Text("%.0f%%", entry.parentPercentage);
                } else if (entry.parentPercentage >= 0.1) {
                    ImGui::Text("%.1f%%", entry.parentPercentage);
                } else {
                    ImGui::Text("<1%%");
                }

                ImGui::TableSetColumnIndex(4);
                if (entry.totalPercentage >= 1.0) {
                    ImGui::Text("%.0f%%", entry.totalPercentage);
                } else if (entry.totalPercentage >= 0.1) {
                    ImGui::Text("%.1f%%", entry.totalPercentage);
                } else {
                    ImGui::Text("<1%%");
                }
            }

            ImGui::EndTable();
        }
    };

    // Render thread section
    renderTreeSection("Render Thread", displayData.renderThread, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));

    // Other threads section
    if (!displayData.otherThreads.empty()) {
        ImGui::Separator();
        renderTreeSection("Other Threads", displayData.otherThreads, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
    }

    ImGui::End();
}

void HandleConfigLoadFailed(HDC hDc, BOOL (*oWglSwapBuffers)(HDC)) {
    if (ImGui::GetCurrentContext() == nullptr) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        (void)io;
        const int screenHeight = GetCachedScreenHeight();
        float scaleFactor = 1.0f;
        if (screenHeight > 1080) { scaleFactor = static_cast<float>(screenHeight) / 1080.0f; }
        scaleFactor = roundf(scaleFactor * 4.0f) / 4.0f;
        if (scaleFactor < 1.0f) { scaleFactor = 1.0f; }

        std::string fontPath = g_config.fontPath;
        const float baseFontSize = 16.0f * scaleFactor;

        auto isStable = [](const std::string& p, float sz) -> bool {
            if (p.empty()) return false;
            ImFontAtlas testAtlas;
            ImFont* f = testAtlas.AddFontFromFileTTF(p.c_str(), sz);
            if (!f) return false;
            return testAtlas.Build();
        };

        std::string usePath = fontPath.empty() ? ConfigDefaults::CONFIG_FONT_PATH : fontPath;
        if (!isStable(usePath, baseFontSize)) { usePath = ConfigDefaults::CONFIG_FONT_PATH; }

        ImFont* baseFont = io.Fonts->AddFontFromFileTTF(usePath.c_str(), baseFontSize);
        if (!baseFont && usePath != ConfigDefaults::CONFIG_FONT_PATH) {
            baseFont = io.Fonts->AddFontFromFileTTF(ConfigDefaults::CONFIG_FONT_PATH.c_str(), baseFontSize);
        }
        if (!baseFont) {
            Log("GUI: Failed to load configured font, using ImGui default font");
            io.Fonts->AddFontDefault();
        }

        ImGui::StyleColorsDark();
        LoadTheme();             // Load theme from theme.toml
        ApplyAppearanceConfig(); // Apply saved theme
        ImGui::GetStyle().ScaleAllSizes(scaleFactor);

        ImGui_ImplWin32_Init(g_minecraftHwnd.load());
        ImGui_ImplOpenGL3_Init("#version 330");

        // Initialize larger font for overlay text labels
        InitializeOverlayTextFont(usePath, 16.0f, scaleFactor);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    RenderConfigErrorGUI();

    ImGui::Render();

    // COMPREHENSIVE OpenGL state protection for ImGui rendering
    {
        // Save comprehensive OpenGL state
        GLint last_program;
        glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
        GLint last_vertex_array;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vertex_array);
        GLint last_array_buffer;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
        GLint last_element_buffer;
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_element_buffer);
        GLint last_texture;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
        GLint last_active_texture;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active_texture);
        GLboolean last_blend = glIsEnabled(GL_BLEND);
        GLint last_blend_src_rgb, last_blend_dst_rgb, last_blend_src_alpha, last_blend_dst_alpha;
        glGetIntegerv(GL_BLEND_SRC_RGB, &last_blend_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &last_blend_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &last_blend_src_alpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &last_blend_dst_alpha);
        GLint last_viewport[4];
        glGetIntegerv(GL_VIEWPORT, last_viewport);
        GLboolean last_depth_test = glIsEnabled(GL_DEPTH_TEST);
        GLboolean last_cull_face = glIsEnabled(GL_CULL_FACE);
        GLboolean last_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
        GLint last_scissor_box[4];
        glGetIntegerv(GL_SCISSOR_BOX, last_scissor_box);
        GLint last_framebuffer;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_framebuffer);

        // Set pixel store parameters for ImGui
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        // Render ImGui with protected state
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Restore ALL OpenGL state after ImGui
        glUseProgram(last_program);
        glBindVertexArray(last_vertex_array);
        glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, last_element_buffer);
        glActiveTexture(last_active_texture);
        glBindTexture(GL_TEXTURE_2D, last_texture);
        if (oglViewport)
            oglViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
        else
            glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
        glScissor(last_scissor_box[0], last_scissor_box[1], last_scissor_box[2], last_scissor_box[3]);
        glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer);

        if (last_depth_test)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        if (last_cull_face)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);
        if (last_scissor_test)
            glEnable(GL_SCISSOR_TEST);
        else
            glDisable(GL_SCISSOR_TEST);

        if (last_blend) {
            glEnable(GL_BLEND);
            glBlendFuncSeparate(last_blend_src_rgb, last_blend_dst_rgb, last_blend_src_alpha, last_blend_dst_alpha);
        } else {
            glDisable(GL_BLEND);
        }
    }
}

void RenderImGuiWithStateProtection(bool useFullProtection) {
    if (useFullProtection) {
        // COMPREHENSIVE OpenGL state protection for ImGui rendering (full GUI)
        // Save comprehensive OpenGL state
        GLint last_program;
        glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
        GLint last_vertex_array;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vertex_array);
        GLint last_array_buffer;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
        GLint last_element_buffer;
        glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &last_element_buffer);
        GLint last_texture;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture);
        GLint last_active_texture;
        glGetIntegerv(GL_ACTIVE_TEXTURE, &last_active_texture);
        GLboolean last_blend = glIsEnabled(GL_BLEND);
        GLint last_blend_src_rgb, last_blend_dst_rgb, last_blend_src_alpha, last_blend_dst_alpha;
        glGetIntegerv(GL_BLEND_SRC_RGB, &last_blend_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &last_blend_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &last_blend_src_alpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &last_blend_dst_alpha);
        GLint last_viewport[4];
        glGetIntegerv(GL_VIEWPORT, last_viewport);
        GLboolean last_depth_test = glIsEnabled(GL_DEPTH_TEST);
        GLboolean last_cull_face = glIsEnabled(GL_CULL_FACE);
        GLboolean last_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
        GLint last_scissor_box[4];
        glGetIntegerv(GL_SCISSOR_BOX, last_scissor_box);
        GLint last_framebuffer;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &last_framebuffer);

        // Set pixel store parameters for ImGui
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);

        // Render ImGui with protected state
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Restore ALL OpenGL state after ImGui
        glUseProgram(last_program);
        glBindVertexArray(last_vertex_array);
        glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, last_element_buffer);
        glActiveTexture(last_active_texture);
        glBindTexture(GL_TEXTURE_2D, last_texture);
        if (oglViewport)
            oglViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
        else
            glViewport(last_viewport[0], last_viewport[1], last_viewport[2], last_viewport[3]);
        glScissor(last_scissor_box[0], last_scissor_box[1], last_scissor_box[2], last_scissor_box[3]);
        glBindFramebuffer(GL_FRAMEBUFFER, last_framebuffer);

        if (last_depth_test)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        if (last_cull_face)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);
        if (last_scissor_test)
            glEnable(GL_SCISSOR_TEST);
        else
            glDisable(GL_SCISSOR_TEST);

        if (last_blend) {
            glEnable(GL_BLEND);
            glBlendFuncSeparate(last_blend_src_rgb, last_blend_dst_rgb, last_blend_src_alpha, last_blend_dst_alpha);
        } else {
            glDisable(GL_BLEND);
        }
    } else {
        // Lightweight state protection for overlays only (performance/profiler)
        // Save minimal critical OpenGL state
        GLint last_program;
        glGetIntegerv(GL_CURRENT_PROGRAM, &last_program);
        GLint last_vertex_array;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &last_vertex_array);
        GLint last_array_buffer;
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &last_array_buffer);
        GLboolean last_blend = glIsEnabled(GL_BLEND);

        // Render ImGui with minimal state protection
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Restore minimal critical state
        glUseProgram(last_program);
        glBindVertexArray(last_vertex_array);
        glBindBuffer(GL_ARRAY_BUFFER, last_array_buffer);
        if (last_blend)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
    }
}
