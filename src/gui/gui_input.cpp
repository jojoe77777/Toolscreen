#include "gui_internal.h"

#include "common/i18n.h"
#include "common/video_media.h"
#include "common/utils.h"
#include "third_party/stb_image.h"

#include <ShlObj.h>
#include <Shlwapi.h>
#include <chrono>
#include <commdlg.h>
#include <filesystem>
#include <future>
#include <map>
#include <windowsx.h>

#pragma comment(lib, "Shlwapi.lib")

static std::atomic<uint64_t> g_bindingInputEventSequence{ 0 };
static std::atomic<DWORD> g_bindingInputEventVk{ 0 };
static std::atomic<LPARAM> g_bindingInputEventLParam{ 0 };
static std::atomic<bool> g_bindingInputEventIsMouse{ false };

static constexpr ULONG_PTR kToolscreenInjectedExtraInfo = (ULONG_PTR)0x5453434E;
static constexpr ULONG_PTR kToolscreenMenuMaskExtraInfo = (ULONG_PTR)0x54534D4B;
static constexpr WORD kToolscreenMenuMaskVk = 0xFF;

static bool IsMenuActivationModifierVk(DWORD vk) {
    return vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}

static bool SendSynthKeyByVirtualKey(WORD virtualKey, bool keyDown, ULONG_PTR extraInfo) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = virtualKey;
    in.ki.wScan = 0;
    in.ki.dwFlags = keyDown ? 0 : KEYEVENTF_KEYUP;
    in.ki.time = 0;
    in.ki.dwExtraInfo = extraInfo;
    return ::SendInput(1, &in, sizeof(INPUT)) == 1;
}

static void SendMenuMaskKeyTap() {
    (void)SendSynthKeyByVirtualKey(kToolscreenMenuMaskVk, true, kToolscreenMenuMaskExtraInfo);
    (void)SendSynthKeyByVirtualKey(kToolscreenMenuMaskVk, false, kToolscreenMenuMaskExtraInfo);
}

static std::mutex g_imagePickerMutex;
static std::map<std::string, ImagePickerResult> g_imagePickerResults;
static std::map<std::string, std::future<ImagePickerResult>> g_imagePickerFutures;
static std::map<std::string, std::string> g_imageErrorMessages;
static std::map<std::string, std::chrono::steady_clock::time_point> g_imageErrorTimes;

int s_mainHotkeyToBind = -1;
int s_sensHotkeyToBind = -1;
int s_keystrokeKeyToBind = -1;
ExclusionBindState s_exclusionToBind = { -1, -1 };
AltBindState s_altHotkeyToBind = { -1, -1 };

static constexpr uint64_t kBindingActiveGraceMs = 250;
static std::atomic<uint64_t> s_lastHotkeyBindingMarkMs{ 0 };
static std::atomic<uint64_t> s_lastRebindBindingMarkMs{ 0 };
static std::atomic<uint64_t> s_lastMirrorColorPickerPopupMarkMs{ 0 };
static std::atomic<bool> s_hotkeyBindingUiActive{ false };
static std::atomic<bool> s_rebindBindingUiActive{ false };

static inline uint64_t NowMs_TickCount64() { return static_cast<uint64_t>(::GetTickCount64()); }

bool IsHotkeyBindingActive_UiState() {
    return s_mainHotkeyToBind != -1 || s_sensHotkeyToBind != -1 || s_keystrokeKeyToBind != -1 || s_exclusionToBind.hotkey_idx != -1 || s_altHotkeyToBind.hotkey_idx != -1;
}

void RegisterBindingInputEvent(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (!IsHotkeyBindingActive_UiState() && !IsHotkeyBindingActive() && !IsRebindBindingActive()) {
        return;
    }

    const ULONG_PTR extraInfo = GetMessageExtraInfo();
    if ((uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN) &&
        (extraInfo == kToolscreenInjectedExtraInfo || extraInfo == kToolscreenMenuMaskExtraInfo)) {
        return;
    }

    DWORD vk = 0;
    bool isMouseButton = false;

    auto resolveVkFromKeyboardMessage = [](WPARAM keyWParam, LPARAM keyLParam) -> DWORD {
        UINT scanCodeWithFlags = static_cast<UINT>((keyLParam >> 16) & 0xFF);
        if ((keyLParam & (1LL << 24)) != 0) { scanCodeWithFlags |= 0xE000; }

        UINT mappedVk = 0;
        if ((scanCodeWithFlags & 0xFF) != 0) { mappedVk = MapVirtualKey(scanCodeWithFlags, MAPVK_VSC_TO_VK_EX); }

        DWORD resolvedVk = static_cast<DWORD>(keyWParam);
        if (mappedVk != 0) { resolvedVk = static_cast<DWORD>(mappedVk); }

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
        if ((lParam & (1LL << 30)) != 0) return;
        vk = resolveVkFromKeyboardMessage(wParam, lParam);
        if (IsMenuActivationModifierVk(vk)) {
            SendMenuMaskKeyTap();
        }
        break;
    case WM_LBUTTONDOWN:
        return;
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
    case WM_MOUSEWHEEL: {
        if (!IsRebindBindingActive()) return;
        const short wheelDelta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (wheelDelta == 0) return;
        vk = (wheelDelta > 0) ? VK_TOOLSCREEN_SCROLL_UP : VK_TOOLSCREEN_SCROLL_DOWN;
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

std::string ValidateImageFile(const std::string& path, const std::wstring& toolscreenPath, int maxDimension) {
    if (path.empty()) { return "Path is empty"; }

    std::wstring final_path;
    std::wstring image_wpath = Utf8ToWide(path);
    if (PathIsRelativeW(image_wpath.c_str()) && !toolscreenPath.empty()) {
        final_path = toolscreenPath + L"\\" + image_wpath;
    } else {
        final_path = image_wpath;
    }

    if (!std::filesystem::exists(final_path)) { return "File does not exist"; }

    std::string path_utf8 = WideToUtf8(final_path);

    int w = 0;
    int h = 0;
    int c = 0;
    if (DetectVisualMediaKindFromPath(path_utf8) == VisualMediaKind::VideoMpeg1) {
        VideoProbeResult probe;
        if (!ProbeMpegVideoFile(path_utf8, probe)) {
            return probe.error.empty() ? "Invalid MPEG-1 video" : probe.error;
        }
        w = probe.width;
        h = probe.height;
    } else if (stbi_info(path_utf8.c_str(), &w, &h, &c) == 0) {
        const char* reason = stbi_failure_reason();
        return std::string("Invalid visual media: ") + (reason ? reason : DescribeSupportedVisualMediaFormats());
    }

    if (w <= 0 || h <= 0) { return "Invalid image dimensions"; }
    if (w > maxDimension || h > maxDimension) {
        return "Image too large (max " + std::to_string(maxDimension) + "x" + std::to_string(maxDimension) + ")";
    }

    return "";
}

ImagePickerResult OpenImagePickerAndValidate(HWND ownerHwnd, const std::wstring& initialDir, const std::wstring& toolscreenPath) {
    ImagePickerResult result;

    HWND safeOwner = NULL;
    if (ownerHwnd != NULL && IsWindow(ownerHwnd)) {
        HWND foreground = GetForegroundWindow();
        DWORD windowThreadId = GetWindowThreadProcessId(ownerHwnd, NULL);
        DWORD currentThreadId = GetCurrentThreadId();

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
        L"Visual Media Files (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.mpg;*.mpeg)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.mpg;*.mpeg\0Image Files (*.png;*.jpg;*.jpeg;*.bmp;*.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0Video Files (*.mpg;*.mpeg)\0*.mpg;*.mpeg\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    std::wstring pickerInitialDir = GetImagePickerInitialDirectory(initialDir);
    ofn.lpstrInitialDir = pickerInitialDir.empty() ? NULL : pickerInitialDir.c_str();
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        result.path = WideToUtf8(ofn.lpstrFile);

        std::string error = ValidateImageFile(result.path, toolscreenPath);
        if (error.empty()) {
            result.success = true;
        } else {
            result.success = false;
            result.error = error;
            result.path.clear();
        }
    } else {
        result.success = false;
    }

    result.completed = true;
    return result;
}

void ClearExpiredImageErrors() {
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

void SetImageError(const std::string& key, const std::string& error) {
    g_imageErrorMessages[key] = error;
    g_imageErrorTimes[key] = std::chrono::steady_clock::now();
}

std::string GetImageError(const std::string& key) {
    ClearExpiredImageErrors();
    auto it = g_imageErrorMessages.find(key);
    return (it != g_imageErrorMessages.end()) ? it->second : "";
}

void ClearImageError(const std::string& key) {
    g_imageErrorMessages.erase(key);
    g_imageErrorTimes.erase(key);
}

void StartAsyncImagePicker(const std::string& pickerId, const std::wstring& initialDir) {
    std::lock_guard<std::mutex> lock(g_imagePickerMutex);

    auto existing = g_imagePickerFutures.find(pickerId);
    if (existing != g_imagePickerFutures.end() &&
        existing->second.valid() &&
        existing->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        return;
    }

    g_imagePickerResults.erase(pickerId);

    const HWND ownerHwnd = g_minecraftHwnd.load(std::memory_order_relaxed);
    const std::wstring toolscreenPath = g_toolscreenPath;
    g_imagePickerFutures[pickerId] = std::async(std::launch::async, [ownerHwnd, initialDir, toolscreenPath]() {
        return OpenImagePickerAndValidate(ownerHwnd, initialDir, toolscreenPath);
    });
}

bool CheckAsyncImagePicker(const std::string& pickerId, std::string& outPath, std::string& outError) {
    std::lock_guard<std::mutex> lock(g_imagePickerMutex);

    auto futureIt = g_imagePickerFutures.find(pickerId);
    if (futureIt != g_imagePickerFutures.end() && futureIt->second.valid() &&
        futureIt->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        g_imagePickerResults[pickerId] = futureIt->second.get();
        g_imagePickerFutures.erase(futureIt);
    }

    auto resultIt = g_imagePickerResults.find(pickerId);
    if (resultIt == g_imagePickerResults.end() || !resultIt->second.completed) { return false; }

    outPath = resultIt->second.path;
    outError = resultIt->second.error;
    g_imagePickerResults.erase(resultIt);
    return true;
}

void HelpMarker(const char* desc) {
    ImGui::TextDisabled(trc("label.question_mark"));
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
        ImGui::TextUnformatted(desc);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void SliderCtrlClickTip() {
    ImGui::TextDisabled(trc("label.tip_right_click_slider"));
    ImGui::Spacing();
}

void RawInputSensitivityNote() {
    ImGui::TextDisabled(trc("label.note_raw_input"));
}

bool IsHotkeyBindingActive() {
    if (g_showGui.load(std::memory_order_acquire) && s_hotkeyBindingUiActive.load(std::memory_order_acquire)) { return true; }
    const uint64_t last = s_lastHotkeyBindingMarkMs.load(std::memory_order_acquire);
    if (last == 0) return false;
    return (NowMs_TickCount64() - last) <= kBindingActiveGraceMs;
}

bool IsRebindBindingActive() {
    if (g_showGui.load(std::memory_order_acquire) && s_rebindBindingUiActive.load(std::memory_order_acquire)) { return true; }
    const uint64_t last = s_lastRebindBindingMarkMs.load(std::memory_order_acquire);
    if (last == 0) return false;
    return (NowMs_TickCount64() - last) <= kBindingActiveGraceMs;
}

bool IsMirrorColorPickerPopupActive() {
    const uint64_t last = s_lastMirrorColorPickerPopupMarkMs.load(std::memory_order_acquire);
    if (!g_showGui.load(std::memory_order_acquire) || last == 0) return false;
    return (NowMs_TickCount64() - last) <= kBindingActiveGraceMs;
}

void ResetTransientBindingUiState() {
    s_hotkeyBindingUiActive.store(false, std::memory_order_release);
    s_rebindBindingUiActive.store(false, std::memory_order_release);
}

void MarkRebindBindingActive() {
    s_rebindBindingUiActive.store(true, std::memory_order_release);
    s_lastRebindBindingMarkMs.store(NowMs_TickCount64(), std::memory_order_release);
}

void MarkMirrorColorPickerPopupActive() {
    s_lastMirrorColorPickerPopupMarkMs.store(NowMs_TickCount64(), std::memory_order_release);
}

void MarkHotkeyBindingActive() {
    s_hotkeyBindingUiActive.store(true, std::memory_order_release);
    s_lastHotkeyBindingMarkMs.store(NowMs_TickCount64(), std::memory_order_release);
}