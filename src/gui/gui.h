#pragma once

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/video_media.h"
#include "config/config_defaults.h"
#include "features/ninjabrain_data.h"
#include "imgui.h"
#include "version.h"

typedef unsigned int GLuint;

struct Color {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

struct DecodedImageData {
    enum Type { Background, UserImage };
    Type type;
    std::string id;
    int width = 0, height = 0, channels = 0;
    unsigned char* data = nullptr;

    bool isAnimated = false;
    bool isVideo = false;
    int frameCount = 0;
    int frameHeight = 0;
    std::vector<int> frameDelays;
};

void ParseColorString(const std::string& input, Color& outColor);
DWORD StringToVk(const std::string& keyStr);
std::string VkToString(DWORD vk);
ImGuiKey VkToImGuiKey(int vk);
void WriteCurrentModeToFile(const std::string& modeId);
void LoadImageAsync(DecodedImageData::Type type, std::string id, std::string path, const std::wstring& toolscreenPath);
std::string WideToUtf8(const std::wstring& wide_string);
void HandleImGuiContextReset();
void InitializeImGuiContext(HWND hwnd);
std::recursive_mutex& GetImGuiContextMutex();
void StartSupportersFetch();
bool IsGuiHotkeyPressed(WPARAM wParam);
bool IsHotkeyBindingActive();
bool IsRebindBindingActive();
bool IsMirrorColorPickerPopupActive();
void ResetTransientBindingUiState();
void MarkRebindBindingActive();
void MarkHotkeyBindingActive();
void MarkMirrorColorPickerPopupActive();
std::vector<DWORD> ParseHotkeyString(const std::string& hotkeyStr);
void RegisterBindingInputEvent(UINT uMsg, WPARAM wParam, LPARAM lParam);
uint64_t GetLatestBindingInputSequence();
bool ConsumeBindingInputEventSince(uint64_t& lastSeenSequence, DWORD& outVk, LPARAM& outLParam, bool& outIsMouseButton);
void RequestDynamicGuiFontRefresh(bool forceRefresh = false);
void ApplyDynamicGuiFontRefresh();
void RequestKeyboardLayoutFontRefresh(const ImVec2& windowSize, float keyHeight, float keyboardScale, bool forceRefresh = false);
void ApplyPendingKeyboardLayoutFontRefresh();

#ifdef TOOLSCREEN_GUI_INTEGRATION_TESTS
struct GuiTestInteractionRect {
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
};

struct GuiTestKeyboardLayoutKeyLabels {
    std::string primaryText;
    std::string secondaryText;
    std::string shiftLayerText;
};

enum class GuiTestKeyboardLayoutDisableTarget {
    None,
    All,
    Types,
    TypesVkShift,
    Triggers,
};

enum class GuiTestKeyboardLayoutBindTarget {
    None,
    FullOutputVk,
    TypesVk,
    TypesVkShift,
    TriggersVk,
};

enum class GuiTestKeyboardLayoutScanFilterGroup {
    All = -1,
    Alpha = 0,
    Digit,
    Function,
    Nav,
    Numpad,
    Modifier,
    Other,
};

enum class GuiTestKeyboardLayoutCursorStateView {
    Any = 0,
    CursorFree,
    CursorGrabbed,
};

void ResetGuiTestInteractionRects();
bool GetGuiTestInteractionRect(const char* id, GuiTestInteractionRect& outRect);
void RecordGuiTestKeyboardLayoutKeyLabels(DWORD vk, const std::string& primaryText, const std::string& secondaryText,
                                          const std::string& shiftLayerText);
bool GetGuiTestKeyboardLayoutKeyLabels(DWORD vk, GuiTestKeyboardLayoutKeyLabels& outLabels);
void RequestGuiTestOpenKeyboardLayout();
void RequestGuiTestOpenKeyboardLayoutContext(DWORD vk);
void RequestGuiTestSetConfigSearchQuery(const std::string& query);
void RequestGuiTestOpenRebindTextOverrideBind(int rebindIndex);
void RequestGuiTestKeyboardLayoutBeginAddCustomBind();
void RequestGuiTestKeyboardLayoutBeginCustomInputCapture();
void RequestGuiTestKeyboardLayoutRemoveCustomKey(DWORD vk);
void RequestGuiTestKeyboardLayoutConfirmRemoveCustomKey();
void RequestGuiTestKeyboardLayoutOpenCustomInputPicker();
void RequestGuiTestKeyboardLayoutSelectCustomInputScan(DWORD scan);
void RequestGuiTestKeyboardLayoutSetDisabledTarget(GuiTestKeyboardLayoutDisableTarget target, bool disabled);
void RequestGuiTestKeyboardLayoutSetOutputDisabled(bool disabled);
void RequestGuiTestKeyboardLayoutSetSplitMode(bool splitMode);
void RequestGuiTestKeyboardLayoutSetScrollWheelEnabled(bool enabled);
void RequestGuiTestKeyboardLayoutSetCursorStateView(GuiTestKeyboardLayoutCursorStateView view);
void RequestGuiTestKeyboardLayoutBeginBind(GuiTestKeyboardLayoutBindTarget target);
void RequestGuiTestKeyboardLayoutSetShiftLayerUppercase(bool enabled);
void RequestGuiTestKeyboardLayoutSetShiftLayerUsesCapsLock(bool enabled);
void RequestGuiTestKeyboardLayoutOpenScanPicker();
void RequestGuiTestKeyboardLayoutSetScanFilter(GuiTestKeyboardLayoutScanFilterGroup group);
void RequestGuiTestKeyboardLayoutSelectScan(DWORD scan);
void RequestGuiTestKeyboardLayoutResetScanToDefault();
#endif

extern ImFont* g_keyboardLayoutPrimaryFont;
extern ImFont* g_keyboardLayoutSecondaryFont;

enum class GradientAnimationType {
    None,
    Rotate,
    Slide,
    Wave,
    Spiral,
    Fade
};

struct GradientColorStop {
    Color color = { 0.0f, 0.0f, 0.0f };
    float position = 0.0f;
};

struct GradientConfig {
    std::vector<GradientColorStop> gradientStops = {
        { { 0.0f, 0.0f, 0.0f }, 0.0f },
        { { 1.0f, 1.0f, 1.0f }, 1.0f },
    };
    float gradientAngle = 0.0f;

    GradientAnimationType gradientAnimation = GradientAnimationType::None;
    float gradientAnimationSpeed = 1.0f;
    bool gradientColorFade = false;
};

struct BackgroundConfig {
    std::string selectedMode = "color";
    std::string image;
    std::string imageFit = ConfigDefaults::BACKGROUND_IMAGE_FIT;
    Color color;

    std::vector<GradientColorStop> gradientStops;
    float gradientAngle = 0.0f;

    GradientAnimationType gradientAnimation = GradientAnimationType::None;
    float gradientAnimationSpeed = 1.0f;
    bool gradientColorFade = false;
};

struct MirrorCaptureConfig {
    int x = 0, y = 0;
    std::string relativeTo = "topLeftScreen";
};
struct MirrorRenderConfig {
    int x = 0, y = 0;
    bool useRelativePosition = false;
    float relativeX = 0.5f;
    float relativeY = 0.5f;
    float scale = 1.0f;
    bool separateScale = false;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    std::string relativeTo = "topLeftScreen";
};
struct MirrorColors {
    std::vector<Color> targetColors;
    Color output, border;
};

enum class MirrorGammaMode {
    Auto = 0,
    AssumeSRGB = 1,
    AssumeLinear = 2
};

enum class MirrorBorderType {
    Dynamic,
    Static
};

enum class MirrorBorderShape {
    Rectangle,
    Circle
};

struct MirrorBorderConfig {
    MirrorBorderType type = MirrorBorderType::Dynamic;

    int dynamicThickness = 1;

    MirrorBorderShape staticShape = MirrorBorderShape::Rectangle;
    Color staticColor = { 1.0f, 1.0f, 1.0f };
    int staticThickness = 2;
    int staticRadius = 0;
    int staticOffsetX = 0;
    int staticOffsetY = 0;
    int staticWidth = 0;
    int staticHeight = 0;
};

struct MirrorConfig {
    std::string name;
    int captureWidth = 50;
    int captureHeight = 50;
    std::vector<MirrorCaptureConfig> input;
    MirrorRenderConfig output;
    MirrorColors colors;
    float colorSensitivity = 0.001f;
    MirrorBorderConfig border;
    int fps = 30;
    float opacity = 1.0f;
    bool rawOutput = false;
    bool colorPassthrough = false;
    bool gradientOutput = false;
    GradientConfig gradient;
    bool onlyOnMyScreen = false;
    bool runtimeGrouped = false;
    std::string runtimeGroupName;
};
struct MirrorGroupItem {
    std::string mirrorId;
    bool enabled = true;
    float widthPercent = 1.0f;
    float heightPercent = 1.0f;
    int offsetX = 0;
    int offsetY = 0;
};
struct MirrorGroupConfig {
    std::string name;
    MirrorRenderConfig output;
    std::vector<MirrorGroupItem> mirrors;
};
struct ImageBackgroundConfig {
    bool enabled = false;
    Color color = { 0.0f, 0.0f, 0.0f };
    float opacity = 1.0f;
};
struct StretchConfig {
    bool enabled = false;
    int width = 0, height = 0, x = 0, y = 0;
};
struct BorderConfig {
    bool enabled = false;
    Color color = { 1.0f, 1.0f, 1.0f };
    int width = 4;
    int radius = 0;
};
struct ColorKeyConfig {
    Color color;
    float sensitivity = 0.05f;
};
struct ImageConfig {
    std::string name;
    std::string path;
    int x = 0, y = 0;
    float scale = 1.0f;
    bool relativeSizing = true;
    int width = 0;
    int height = 0;
    std::string relativeTo = "topLeftScreen";
    int crop_top = 0, crop_bottom = 0, crop_left = 0, crop_right = 0;
    bool cropToWidth = false;
    bool cropToHeight = false;
    bool enableColorKey = false;
    std::vector<ColorKeyConfig> colorKeys;
    Color colorKey;
    float colorKeySensitivity = 0.001f;
    float opacity = 1.0f;
    ImageBackgroundConfig background;
    bool pixelatedScaling = false;
    bool onlyOnMyScreen = false;
    BorderConfig border;
};
struct WindowOverlayConfig {
    std::string name;
    std::string windowTitle;
    std::string windowClass;
    std::string executableName;
    std::string windowMatchPriority = "title";
    int x = 0, y = 0;
    float scale = 1.0f;
    bool separateScale = false;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    std::string relativeTo = "topLeftScreen";
    int crop_top = 0, crop_bottom = 0, crop_left = 0, crop_right = 0;
    bool cropToWidth = false;
    bool cropToHeight = false;
    bool enableColorKey = false;
    std::vector<ColorKeyConfig> colorKeys;
    Color colorKey;
    float colorKeySensitivity = 0.001f;
    float opacity = 1.0f;
    ImageBackgroundConfig background;
    bool pixelatedScaling = false;
    bool onlyOnMyScreen = false;
    int fps = 30;
    int searchInterval = 1000;
    std::string captureMethod = "Windows 10+"; // Capture method: "Windows 10+" (default) or "BitBlt"
    bool forceUpdate = false;
    bool enableInteraction = false;
    BorderConfig border;
};
struct BrowserOverlayConfig {
    std::string name;
    std::string url = "https://example.com";
    std::string customCss;
    int browserWidth = 1280;
    int browserHeight = 720;
    int x = 0, y = 0;
    float scale = 1.0f;
    std::string relativeTo = "topLeftScreen";
    int crop_top = 0, crop_bottom = 0, crop_left = 0, crop_right = 0;
    bool cropToWidth = false;
    bool cropToHeight = false;
    bool enableColorKey = false;
    std::vector<ColorKeyConfig> colorKeys;
    float opacity = 1.0f;
    ImageBackgroundConfig background;
    bool pixelatedScaling = false;
    bool onlyOnMyScreen = false;
    int fps = 15;
    bool transparentBackground = false;
    bool muteAudio = true;
    bool hardwareAcceleration = true;
    bool allowSystemMediaKeys = true;
    bool reloadOnUpdate = false;
    int reloadInterval = 0;
    BorderConfig border;
};

struct ResolvedCrop {
    int top, bottom, left, right;
};

inline ResolvedCrop ResolveCrop(int crop_top, int crop_bottom, int crop_left, int crop_right,
                                bool cropToWidth, bool cropToHeight, int sourceW, int sourceH) {
    int rt = crop_top, rb = crop_bottom, rl = crop_left, rr = crop_right;
    if (cropToHeight && sourceH > 0) rb = (std::max)(0, sourceH - crop_top - crop_bottom);
    if (cropToWidth && sourceW > 0) rr = (std::max)(0, sourceW - crop_left - crop_right);
    return { rt, rb, rl, rr };
}

enum class GameTransitionType {
    Cut,
    Bounce
};

enum class OverlayTransitionType {
    Cut
};

enum class BackgroundTransitionType {
    Cut
};

enum class EasingType {
    Linear,
    EaseOut,
    EaseIn,
    EaseInOut
};

enum class ModeSourceType {
    Mirror,
    MirrorGroup,
    Image,
    WindowOverlay,
    BrowserOverlay
};

struct ModeSourceRef {
    ModeSourceType type = ModeSourceType::Mirror;
    std::string id;
};

struct ModeConfig {
    std::string id;
    int width = 0, height = 0;
    int manualWidth = 0, manualHeight = 0;
    bool useRelativeSize = false;
    float relativeWidth = 0.5f;
    float relativeHeight = 0.5f;
    std::string widthExpr;
    std::string heightExpr;

    BackgroundConfig background;
    std::vector<ModeSourceRef> sources;
    StretchConfig stretch;

    GameTransitionType gameTransition = GameTransitionType::Cut;
    OverlayTransitionType overlayTransition = OverlayTransitionType::Cut;
    BackgroundTransitionType backgroundTransition = BackgroundTransitionType::Cut;
    int transitionDurationMs = 150;

    float easeInPower = 1.0f;
    float easeOutPower = 3.0f;
    int bounceCount = 0;
    float bounceIntensity = 0.15f;
    int bounceDurationMs = 100;
    bool relativeStretching = false;
    bool skipAnimateX = false;
    bool skipAnimateY = false;

    BorderConfig border;

    bool sensitivityOverrideEnabled = false;
    float modeSensitivity = 1.0f;
    bool separateXYSensitivity = false;
    float modeSensitivityX = 1.0f;
    float modeSensitivityY = 1.0f;

    bool slideMirrorsIn = false;
};
struct HotkeyConditions {
    std::vector<std::string> gameState;
    std::vector<DWORD> exclusions;
};
struct AltSecondaryMode {
    std::vector<DWORD> keys;
    std::string mode;
};
struct HotkeyConfig {
    std::vector<DWORD> keys;

    std::string mainMode;
    std::string secondaryMode;
    std::vector<AltSecondaryMode> altSecondaryModes;

    HotkeyConditions conditions;
    int debounce = 100;
    bool triggerOnRelease = false; // When true, hotkey triggers on key release instead of key press
    bool triggerOnHold = false;    // When true, hotkey activates on key press and deactivates on key release

    bool blockKeyFromGame = false;

    bool allowExitToFullscreenRegardlessOfGameState = false;
};

struct SensitivityHotkeyConfig {
    std::vector<DWORD> keys;
    float sensitivity = 1.0f;
    bool separateXY = false;
    float sensitivityX = 1.0f;
    float sensitivityY = 1.0f;
    bool toggle = false;
    HotkeyConditions conditions;
    int debounce = 100;
};
struct DebugGlobalConfig {
    bool showPerformanceOverlay = false;
    bool showProfiler = false;
    float profilerScale = 0.8f;
    bool showHotkeyDebug = false;
    bool fakeCursor = false;
    bool showTextureGrid = false;
    bool delayRenderingUntilFinished = false;
    bool virtualCameraEnabled = false;        // Output to OBS Virtual Camera driver
    int videoCacheBudgetMiB = ConfigDefaults::DEBUG_GLOBAL_VIDEO_CACHE_BUDGET_MIB;

    bool logModeSwitch = false;
    bool logAnimation = false;
    bool logHotkey = false;
    bool logObs = false;
    bool logWindowOverlay = false;
    bool logBrowserOverlay = false;
    bool logNinjabrain = false;
    bool logFileMonitor = false;
    bool logImageMonitor = false;
    bool logPerformance = false;
    bool logTextureOps = false;
    bool logGui = false;
    bool logInit = false;
    bool logCursorTextures = false;
};
struct CursorConfig {
    std::string cursorName = "";
    int cursorSize = 64;
};
struct CursorsConfig {
    bool enabled = false;
    CursorConfig title;
    CursorConfig wall;
    CursorConfig ingame;
};
struct CursorTrailConfig {
    bool enabled = ConfigDefaults::CURSOR_TRAIL_ENABLED;
    bool onlyOnMyScreen = ConfigDefaults::CURSOR_TRAIL_ONLY_ON_MY_SCREEN;
    bool onlyOnObs = ConfigDefaults::CURSOR_TRAIL_ONLY_ON_OBS;
    int lifetimeMs = ConfigDefaults::CURSOR_TRAIL_LIFETIME_MS;
    int stampSpacingPx = ConfigDefaults::CURSOR_TRAIL_STAMP_SPACING_PX;
    int spriteSizePx = ConfigDefaults::CURSOR_TRAIL_SPRITE_SIZE_PX;
    float tailSizeScale = ConfigDefaults::CURSOR_TRAIL_TAIL_SIZE_SCALE;
    bool useVelocitySize = ConfigDefaults::CURSOR_TRAIL_USE_VELOCITY_SIZE;
    float velocitySizeIntensity = ConfigDefaults::CURSOR_TRAIL_VELOCITY_SIZE_INTENSITY;
    Color color = { ConfigDefaults::CURSOR_TRAIL_COLOR_R, ConfigDefaults::CURSOR_TRAIL_COLOR_G, ConfigDefaults::CURSOR_TRAIL_COLOR_B };
    bool useGradient = ConfigDefaults::CURSOR_TRAIL_USE_GRADIENT;
    Color tailColor = { ConfigDefaults::CURSOR_TRAIL_TAIL_COLOR_R, ConfigDefaults::CURSOR_TRAIL_TAIL_COLOR_G, ConfigDefaults::CURSOR_TRAIL_TAIL_COLOR_B };
    float opacity = ConfigDefaults::CURSOR_TRAIL_OPACITY;
    std::string blendMode = ConfigDefaults::CURSOR_TRAIL_BLEND_MODE;
    std::string spritePath = ConfigDefaults::CURSOR_TRAIL_SPRITE_PATH;
};
enum class EyeZoomOverlayDisplayMode { Manual, Fit, Stretch };

enum class EyeZoomFontSizeMode {
    Auto,
    PerSquareAuto,
    Manual
};

struct EyeZoomOverlayConfig {
    std::string name;
    std::string path;
    EyeZoomOverlayDisplayMode displayMode = EyeZoomOverlayDisplayMode::Fit;
    int manualWidth = 100;
    int manualHeight = 100;
    bool clipToZoomArea = false;
    float opacity = 1.0f;
};

struct EyeZoomConfig {
    int cloneWidth = 24;
    int overlayWidth = 12;
    int cloneHeight = 2080;
    int stretchWidth = 810;
    int windowWidth = 384;
    int windowHeight = 16384;
    int zoomAreaWidth = 0;
    int zoomAreaHeight = 0;
    bool useCustomSizePosition = false;
    int positionX = 0;
    int positionY = 0;
    EyeZoomFontSizeMode fontSizeMode = static_cast<EyeZoomFontSizeMode>(ConfigDefaults::EYEZOOM_FONT_SIZE_MODE);
    int textFontSize = 24;
    std::string textFontPath;
    int rectHeight = 24;
    bool linkRectToFont = true;
    Color gridColor1 = { 1.0f, 0.714f, 0.757f };
    float gridColor1Opacity = 1.0f;
    Color gridColor2 = { 0.678f, 0.847f, 0.902f };
    float gridColor2Opacity = 1.0f;
    Color centerLineColor = { 1.0f, 1.0f, 1.0f };
    float centerLineColorOpacity = 1.0f;
    Color textColor = { 0.0f, 0.0f, 0.0f };
    float textColorOpacity = 1.0f;
    bool slideZoomIn = false;
    bool slideMirrorsIn = false;
    int activeOverlayIndex = -1; // -1 = Default (numbered boxes), 0+ = custom overlay index
    std::vector<EyeZoomOverlayConfig> overlays;
};
struct AppearanceConfig {
    std::string theme = "Dark";
    float guiFontScale = ConfigDefaults::CONFIG_GUI_FONT_SCALE;
    std::map<std::string, Color> customColors;
};

inline constexpr DWORD VK_TOOLSCREEN_SCROLL_UP = 0x1000;
inline constexpr DWORD VK_TOOLSCREEN_SCROLL_DOWN = 0x1001;

inline constexpr const char* kKeyRebindCursorStateAny = "any";
inline constexpr const char* kKeyRebindCursorStateCursorFree = "cursor_free";
inline constexpr const char* kKeyRebindCursorStateCursorGrabbed = "cursor_grabbed";

inline std::string NormalizeKeyRebindCursorStateId(std::string cursorState) {
    std::transform(cursorState.begin(), cursorState.end(), cursorState.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (cursorState == kKeyRebindCursorStateCursorFree) {
        return kKeyRebindCursorStateCursorFree;
    }
    if (cursorState == kKeyRebindCursorStateCursorGrabbed) {
        return kKeyRebindCursorStateCursorGrabbed;
    }
    return kKeyRebindCursorStateAny;
}

struct KeyRebind {
    DWORD fromKey = 0;
    DWORD toKey = 0;
    bool enabled = true;
    std::string cursorState = kKeyRebindCursorStateAny;

    bool triggerOutputDisabled = false;
    bool useCustomOutput = false;
    bool baseOutputDisabled = false;
    DWORD customOutputVK = 0;
    DWORD customOutputUnicode = 0;
    DWORD customOutputScanCode = 0;
    bool baseOutputShifted = false;
    bool shiftLayerEnabled = false;
    bool shiftLayerUsesCapsLock = false;
    bool shiftLayerOutputDisabled = false;
    DWORD shiftLayerOutputVK = 0;
    DWORD shiftLayerOutputUnicode = 0;
    bool shiftLayerOutputShifted = false;
};
struct KeyRebindsConfig {
    bool enabled = false;
    bool resolveRebindTargetsForHotkeys = ConfigDefaults::KEY_REBINDS_RESOLVE_REBIND_TARGETS_FOR_HOTKEYS;
    bool allowSystemAltTab = ConfigDefaults::KEY_REBINDS_ALLOW_SYSTEM_ALT_TAB;
    int indicatorMode = ConfigDefaults::KEY_REBINDS_INDICATOR_MODE;
    int indicatorPosition = ConfigDefaults::KEY_REBINDS_INDICATOR_POSITION;
    std::string indicatorImageEnabled;
    std::string indicatorImageDisabled;
    bool allowSystemAltF4 = ConfigDefaults::KEY_REBINDS_ALLOW_SYSTEM_ALT_F4;
    bool suppressCapsLockToggle = ConfigDefaults::KEY_REBINDS_SUPPRESS_CAPS_LOCK_TOGGLE;
    std::vector<DWORD> toggleHotkey = {};
    std::vector<DWORD> layoutExtraKeys;
    std::vector<KeyRebind> rebinds;
};

inline bool IsKeyRebindMouseButtonVk(DWORD vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

inline bool IsKeyRebindMouseWheelPseudoVk(DWORD vk) {
    return vk == VK_TOOLSCREEN_SCROLL_UP || vk == VK_TOOLSCREEN_SCROLL_DOWN;
}

inline bool IsKeyRebindMouseLikeVk(DWORD vk) {
    return IsKeyRebindMouseButtonVk(vk) || IsKeyRebindMouseWheelPseudoVk(vk);
}

inline bool IsKeyRebindModifierVk(DWORD vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}

inline DWORD NormalizeKeyRebindModifierVkFromConfig(DWORD vk, UINT scanCodeWithFlags = 0) {
    const UINT scanLow = scanCodeWithFlags & 0xFF;
    const bool isExtended = (scanCodeWithFlags & 0xFF00) != 0;

    switch (vk) {
    case VK_SHIFT:
        if (scanLow == 0x36) return VK_RSHIFT;
        return VK_LSHIFT;
    case VK_CONTROL:
        return isExtended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU:
        return isExtended ? VK_RMENU : VK_LMENU;
    default:
        return vk;
    }
}

inline DWORD GetKeyRebindScanCodeWithExtendedFlag(DWORD vkCode) {
    auto isExtendedVk = [](DWORD vk) {
        switch (vk) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_INSERT:
        case VK_DELETE:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_RCONTROL:
        case VK_RMENU:
        case VK_DIVIDE:
        case VK_NUMLOCK:
        case VK_SNAPSHOT:
            return true;
        default:
            return false;
        }
    };

    UINT scanCodeWithFlags = MapVirtualKeyW(static_cast<UINT>(vkCode), MAPVK_VK_TO_VSC_EX);
    if (scanCodeWithFlags == 0) {
        scanCodeWithFlags = MapVirtualKeyW(static_cast<UINT>(vkCode), MAPVK_VK_TO_VSC);
    }

    if ((scanCodeWithFlags & 0xFF00) == 0 && isExtendedVk(vkCode) && (scanCodeWithFlags & 0xFF) != 0) {
        scanCodeWithFlags |= 0xE000;
    }

    return static_cast<DWORD>(scanCodeWithFlags);
}

inline DWORD ResolveKeyRebindModifierVkFromScanCode(UINT scanCodeWithFlags) {
    const UINT scanLow = scanCodeWithFlags & 0xFF;
    if (scanLow == 0) return 0;

    DWORD mappedVk = static_cast<DWORD>(MapVirtualKeyW(scanCodeWithFlags, MAPVK_VSC_TO_VK_EX));
    if (mappedVk == 0 && (scanCodeWithFlags & 0xFF00) != 0) {
        mappedVk = static_cast<DWORD>(MapVirtualKeyW(scanLow, MAPVK_VSC_TO_VK_EX));
    }
    if (mappedVk != 0) {
        mappedVk = NormalizeKeyRebindModifierVkFromConfig(mappedVk, scanCodeWithFlags);
    }
    if (!IsKeyRebindModifierVk(mappedVk)) {
        return 0;
    }
    return mappedVk;
}

inline bool IsKeyRebindModifierScanCode(UINT scanCodeWithFlags) {
    return ResolveKeyRebindModifierVkFromScanCode(scanCodeWithFlags) != 0;
}

inline bool IsKeyRebindConsumeOnly(const KeyRebind& rebind) {
    return rebind.enabled && rebind.fromKey != 0 && rebind.toKey == 0;
}

inline bool IsKeyRebindTriggerOutputDisabled(const KeyRebind& rebind) {
    return !IsKeyRebindConsumeOnly(rebind) && rebind.triggerOutputDisabled;
}

inline bool DoesKeyRebindTriggerCannotType(const KeyRebind& rebind) {
    if (IsKeyRebindTriggerOutputDisabled(rebind)) {
        return false;
    }

    DWORD triggerVk = rebind.toKey;
    if (triggerVk == 0) triggerVk = rebind.fromKey;
    if (triggerVk == 0) return false;

    UINT triggerScan = (rebind.useCustomOutput && rebind.customOutputScanCode != 0)
        ? static_cast<UINT>(rebind.customOutputScanCode)
        : static_cast<UINT>(GetKeyRebindScanCodeWithExtendedFlag(triggerVk));

    if (triggerScan != 0 && (triggerScan & 0xFF00) == 0) {
        const UINT vkScan = static_cast<UINT>(GetKeyRebindScanCodeWithExtendedFlag(triggerVk));
        if ((vkScan & 0xFF00) != 0 && ((vkScan & 0xFF) == (triggerScan & 0xFF))) {
            triggerScan = vkScan;
        }
    }

    if (IsKeyRebindModifierVk(triggerVk) || IsKeyRebindModifierScanCode(triggerScan)) return true;
    if (IsKeyRebindMouseLikeVk(triggerVk)) return true;

    switch (triggerVk) {
    case VK_BACK:
    case VK_CAPITAL:
    case VK_DELETE:
    case VK_HOME:
    case VK_INSERT:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
        return true;
    default:
        return false;
    }
}

inline bool SyncKeyRebindUseCustomOutput(KeyRebind& rebind) {
    const bool desiredUseCustomOutput = rebind.customOutputVK != 0 || rebind.customOutputUnicode != 0 || rebind.customOutputScanCode != 0;
    if (rebind.useCustomOutput == desiredUseCustomOutput) {
        return false;
    }
    rebind.useCustomOutput = desiredUseCustomOutput;
    return true;
}

inline bool SanitizeKeyRebindTypedOutputsForCannotTypeTrigger(KeyRebind& rebind) {
    if (!DoesKeyRebindTriggerCannotType(rebind)) {
        return false;
    }

    bool changed = false;
    auto clearBool = [&](bool& value) {
        if (value) {
            value = false;
            changed = true;
        }
    };
    auto clearDword = [&](DWORD& value) {
        if (value != 0) {
            value = 0;
            changed = true;
        }
    };

    clearBool(rebind.baseOutputDisabled);
    clearDword(rebind.customOutputVK);
    clearDword(rebind.customOutputUnicode);
    clearBool(rebind.baseOutputShifted);
    clearBool(rebind.shiftLayerEnabled);
    clearBool(rebind.shiftLayerUsesCapsLock);
    clearBool(rebind.shiftLayerOutputDisabled);
    clearDword(rebind.shiftLayerOutputVK);
    clearDword(rebind.shiftLayerOutputUnicode);
    clearBool(rebind.shiftLayerOutputShifted);

    if (SyncKeyRebindUseCustomOutput(rebind)) {
        changed = true;
    }

    return changed;
}

inline bool SanitizeKeyRebindsForCannotTypeTriggers(KeyRebindsConfig& config) {
    bool changed = false;
    for (KeyRebind& rebind : config.rebinds) {
        if (SanitizeKeyRebindTypedOutputsForCannotTypeTrigger(rebind)) {
            changed = true;
        }
    }
    return changed;
}

// NinjabrainBot overlay.
struct NinjabrainColumn {
    std::string id;
    std::string header;
    bool show = true;
    int staticWidth = 0;
};

constexpr float kNinjabrainOverlayBaseFontSize = 64.0f;

struct NinjabrainOverlayConfig {
    bool enabled = false;
    int x = 4;
    int y = -5;
    std::string relativeTo = "bottomLeftScreen";
    std::string customFontPath = ConfigDefaults::CONFIG_FONT_PATH;
    std::string apiBaseUrl = ConfigDefaults::CONFIG_NINJABRAIN_API_BASE_URL;
    bool fontAntialiasing = true;
    bool bgEnabled = true;
    float bgOpacity = 1.0f;
    Color bgColor = { 0.2157f, 0.2353f, 0.2588f, 1.0f };
    std::string layoutStyle = "compact";
    std::string titleText = "Ninjabrain Bot";
    bool showTitleBar = false;
    bool showWindowControls = false;
    bool showThrowDetails = true;
    bool showDirectionToStronghold = true;
    bool staticColumnWidths = true;
    bool showSeparators = true;
    bool showRowStripes = true;
    int borderWidth = 0;
    float borderRadius = 0.0f;
    float cornerRadius = 0.0f;
    Color headerFillColor = { 0.1765f, 0.1961f, 0.2196f, 1.0f };
    std::string coordsDisplay = "chunk";
    Color chromeColor = { 0.1804f, 0.2000f, 0.2392f, 1.0f };
    Color borderColor = { 0.2784f, 0.2902f, 0.3098f, 1.0f };
    Color dividerColor = { 0.1647f, 0.1804f, 0.1961f, 1.0f };
    Color headerDividerColor = { 0.1294f, 0.1451f, 0.1608f, 1.0f };
    Color accentColor = { 0.0f, 1.0f, 0.0f, 1.0f };
    Color buttonColor = { 0.2314f, 0.2588f, 0.3020f, 1.0f };
    int outlineWidth = 0;
    Color outlineColor = { 0.0f, 0.0f, 0.0f, 0.8627f };
    Color textColor = { 0.8980f, 0.8980f, 0.8980f, 1.0f };
    Color dataColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    Color titleTextColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    Color throwsTextColor = { 0.7529f, 0.7529f, 0.7529f, 1.0f };
    Color divineTextColor = { 0.8980f, 0.8980f, 0.8980f, 1.0f };
    Color versionTextColor = { 0.7608f, 0.7608f, 0.7608f, 1.0f };
    Color throwsBackgroundColor = { 0.2000f, 0.2196f, 0.2392f, 1.0f };
    Color coordPositiveColor = { 1.0f, 1.0f, 1.0f, 1.0f };
    Color coordNegativeColor = { 1.0f, 0.4510f, 0.4510f, 1.0f };
    Color certaintyColor = { 0.0f, 0.8078f, 0.1608f, 1.0f };
    Color certaintyMidColor = { 1.0f, 1.0f, 0.0f, 1.0f };
    Color certaintyLowColor = { 1.0f, 0.0f, 0.0f, 1.0f };
    Color subpixelPositiveColor = { 0.4588f, 0.8000f, 0.4235f, 1.0f };
    Color subpixelNegativeColor = { 0.8000f, 0.4314f, 0.4471f, 1.0f };
    std::vector<std::string> allowedModes;
    float overlayOpacity = 1.0f;
    float overlayScale = 0.56f;
    bool onlyOnMyScreen = false;
    bool onlyOnObs = false;
    bool hideIfStale = false;
    int hideIfStaleDelaySeconds = 30;
    bool showEyeOverlay = true;
    int shownPredictions = 5;
    bool showAllPreds = false;
    bool alwaysShow = false;
    int angleDisplay = 1;
    float rowSpacing = 4.0f;
    float colSpacing = 36.0f;
    float sidePadding = 20.0f;
    std::string sectionLayoutMode = "flow";
    float contentPaddingTop = 0.0f;
    float contentPaddingBottom = 0.0f;
    float resultsMarginLeft = 0.0f;
    float resultsMarginRight = 0.0f;
    float resultsMarginTop = 0.0f;
    float resultsMarginBottom = 0.0f;
    float resultsHeaderPaddingY = 2.0f;
    float resultsColumnGap = 0.0f;
    std::string resultsAnchor = "topLeft";
    float resultsOffsetX = 0.0f;
    float resultsOffsetY = 0.0f;
    int resultsDrawOrder = 0;
    std::string informationMessagesPlacement = "middle";
    float informationMessagesFontScale = 1.0f;
    float informationMessagesMinWidth = 420.0f;
    float informationMessagesMarginLeft = 0.0f;
    float informationMessagesMarginRight = 0.0f;
    float informationMessagesMarginTop = 0.0f;
    float informationMessagesMarginBottom = 0.0f;
    float informationMessagesIconTextMargin = 8.0f;
    float informationMessagesIconScale = 1.0f;
    std::string informationMessagesAnchor = "topLeft";
    float informationMessagesOffsetX = 0.0f;
    float informationMessagesOffsetY = 0.0f;
    int informationMessagesDrawOrder = 1;
    float throwsMarginLeft = 0.0f;
    float throwsMarginRight = 0.0f;
    float throwsMarginTop = 4.0f;
    float throwsMarginBottom = 0.0f;
    float throwsHeaderPaddingY = 3.0f;
    float throwsRowPaddingY = 3.0f;
    int eyeThrowRows = 3;
    std::string throwsAnchor = "topLeft";
    float throwsOffsetX = 0.0f;
    float throwsOffsetY = 0.0f;
    int throwsDrawOrder = 2;
    float failureMarginLeft = 0.0f;
    float failureMarginRight = 0.0f;
    float failureMarginTop = 0.0f;
    float failureMarginBottom = 0.0f;
    float failureLineGap = 8.0f;
    std::string failureAnchor = "topLeft";
    float failureOffsetX = 0.0f;
    float failureOffsetY = 0.0f;
    int failureDrawOrder = 0;
    float blindMarginLeft = 0.0f;
    float blindMarginRight = 0.0f;
    float blindMarginTop = 0.0f;
    float blindMarginBottom = 0.0f;
    float blindLineGap = 8.0f;
    std::string blindAnchor = "topLeft";
    float blindOffsetX = 0.0f;
    float blindOffsetY = 0.0f;
    int blindDrawOrder = 0;
    bool alwaysShowBoat = false;
    bool showBoatStateInTopBar = false;
    float boatStateSize = 64.0f;
    float boatStateMarginRight = 8.0f;
    std::vector<NinjabrainColumn> columns = {
        {"coords", "Chunk", true},
        {"certainty", "%",        true},
        {"distance", "Dist.", true},
        {"nether", "Nether", true},
        {"angle", "Angle", true},
    };
};
struct Config {
    int configVersion = GetConfigVersion();
    std::vector<MirrorConfig> mirrors;
    std::vector<MirrorGroupConfig> mirrorGroups;
    std::vector<ImageConfig> images;
    std::vector<WindowOverlayConfig> windowOverlays;
    std::vector<BrowserOverlayConfig> browserOverlays;
    std::vector<ModeConfig> modes;
    std::vector<HotkeyConfig> hotkeys;
    std::vector<SensitivityHotkeyConfig> sensitivityHotkeys;
    EyeZoomConfig eyezoom;
    std::string defaultMode = "fullscreen";
    DebugGlobalConfig debug;
    std::vector<DWORD> guiHotkey = ConfigDefaults::GetDefaultGuiHotkey();
    std::vector<DWORD> borderlessHotkey = {};
    bool autoBorderless = false;
    std::vector<DWORD> imageOverlaysHotkey = {};
    std::vector<DWORD> windowOverlaysHotkey = {};
    std::vector<DWORD> ninjabrainOverlayHotkey = {};
    CursorsConfig cursors;
    CursorTrailConfig cursorTrail;
    std::string fontPath = ConfigDefaults::CONFIG_DEFAULT_GUI_FONT_PATH;
    std::string lang = "en";
    int fpsLimit = 0;
    int fpsLimitSleepThreshold = 1000;
    MirrorGammaMode mirrorGammaMode = MirrorGammaMode::Auto;
    // Useful if a specific overlay/driver hook layer is unstable when chained.
    bool disableHookChaining = false;
    bool allowCursorEscape = false;
    bool confineCursor = false;
    float mouseSensitivity = 1.0f;
    int windowsMouseSpeed = 0;                              // Windows mouse speed override (0 = disabled, 1-20 = override)
    bool hideAnimationsInGame = false;
    bool captureFakeCursor = ConfigDefaults::CONFIG_CAPTURE_FAKE_CURSOR;
    bool limitCaptureFramerate = ConfigDefaults::CONFIG_LIMIT_CAPTURE_FRAMERATE;
    int obsFramerate = ConfigDefaults::CONFIG_OBS_FRAMERATE;
    KeyRebindsConfig keyRebinds;
    AppearanceConfig appearance;
    bool useSystemKeyRepeat = ConfigDefaults::CONFIG_USE_SYSTEM_KEY_REPEAT;
    int keyRepeatStartDelay = ConfigDefaults::CONFIG_KEY_REPEAT_START_DELAY;
    int keyRepeatDelay = ConfigDefaults::CONFIG_KEY_REPEAT_DELAY;
    bool basicModeEnabled = false;
    bool restoreWindowedModeOnFullscreenExit = ConfigDefaults::CONFIG_RESTORE_WINDOWED_MODE_ON_FULLSCREEN_EXIT;
    bool disableFullscreenPrompt = false;
    bool disableConfigurePrompt = false;
    NinjabrainOverlayConfig ninjabrainOverlay;
};

inline bool SanitizeConfigKeyRebindsForCannotTypeTriggers(Config& config) {
    return SanitizeKeyRebindsForCannotTypeTriggers(config.keyRebinds);
}

inline constexpr const char* kDefaultProfileName = "Default";
inline constexpr float kDefaultProfileColor[3] = { 0.4f, 0.8f, 0.4f };

struct ProfileSectionSelection {
    bool modes = true;
    bool mirrors = true;
    bool images = true;
    bool windowOverlays = true;
    bool browserOverlays = true;
    bool ninjabrainOverlay = true;
    bool hotkeys = true;
    bool inputsMouse = true;
    bool captureWindow = true;
    bool settings = true;
    bool appearance = true;

    bool operator==(const ProfileSectionSelection&) const = default;
};

struct ProfileMetadata {
    std::string name;
    float color[3] = { kDefaultProfileColor[0], kDefaultProfileColor[1], kDefaultProfileColor[2] };
    ProfileSectionSelection sections;
};

struct ProfilesConfig {
    std::string activeProfile = kDefaultProfileName;
    std::vector<ProfileMetadata> profiles;
};

extern ProfilesConfig g_profilesConfig;

bool IsValidProfileName(const std::string& name);
void SaveProfile(const std::string& name);
bool LoadProfile(const std::string& name);
void SwitchProfile(const std::string& newProfileName);
void ApplyProfileFields(const Config& src, Config& dst);
void ApplyProfileFields(const Config& src, Config& dst, const ProfileSectionSelection& sections);
bool LoadProfilesConfig();
void SaveProfilesConfig();
bool EnsureProfilesConfigReady();
bool CreateNewProfile(const std::string& name);
bool DuplicateProfile(const std::string& srcName, const std::string& dstName);
void DeleteProfile(const std::string& name);
bool RenameProfile(const std::string& oldName, const std::string& newName);
bool UpdateProfileMetadata(const std::string& currentName, const std::string& newName, const float color[3],
                           const ProfileSectionSelection& sections);
bool SaveProfileSnapshot(const std::string& name, const Config& configSnapshot);
bool SaveProfileSnapshotIfTracked(const std::string& name, const Config& configSnapshot);
bool MigrateToProfiles();

struct GameViewportGeometry {
    int gameW = 0, gameH = 0;
    int finalX = 0, finalY = 0, finalW = 0, finalH = 0;
};

struct ModeTransitionAnimation {
    bool active = false;
    std::chrono::steady_clock::time_point startTime;
    float duration = 0.3f;

    GameTransitionType gameTransition = GameTransitionType::Cut;
    OverlayTransitionType overlayTransition = OverlayTransitionType::Cut;
    BackgroundTransitionType backgroundTransition = BackgroundTransitionType::Cut;

    float easeInPower = 1.0f;
    float easeOutPower = 3.0f;
    int bounceCount = 0;
    float bounceIntensity = 0.15f;
    int bounceDurationMs = 100;
    bool skipAnimateX = false;
    bool skipAnimateY = false;

    std::string fromModeId;
    int fromWidth = 0;
    int fromHeight = 0;
    int fromX = 0;
    int fromY = 0;

    std::string toModeId;
    int toWidth = 0;
    int toHeight = 0;
    int toX = 0;
    int toY = 0;

    int fromNativeWidth = 0;
    int fromNativeHeight = 0;
    int toNativeWidth = 0;
    int toNativeHeight = 0;

    int currentWidth = 0;
    int currentHeight = 0;
    int currentX = 0;
    int currentY = 0;
    float progress = 0.0f;
    float moveProgress = 0.0f;

    int lastSentWidth = 0;
    int lastSentHeight = 0;

    bool wmSizeSent = false;
};

extern Config g_config;
extern Config g_sharedConfig;
extern std::atomic<bool> g_configIsDirty;

// g_config is the mutable draft, only touched by the GUI/main thread.
// After any mutation, call PublishConfigSnapshot() to atomically publish an
// immutable snapshot. Reader threads call GetConfigSnapshot() to get a
// shared_ptr<const Config> they can safely use without locking.
// Hot-path readers (render thread, logic thread, input hook) grab a snapshot
// once per frame/tick and work from that — zero contention, zero mutex.

// Atomically publish current g_config as an immutable snapshot.
void PublishConfigSnapshot();

// Synchronize the shared/base GUI config from the current draft and publish
// the merged runtime snapshot for reader threads.
void PublishGuiConfigSnapshot();

// Atomically publish a specific config instance as an immutable snapshot.
// Use this for derived/runtime snapshots built off the latest published config.
void PublishConfigSnapshot(const Config& config);

// Get the latest published config snapshot. Lock-free, safe from any thread.
std::shared_ptr<const Config> GetConfigSnapshot();

// Lock-free read of the published current mode id.
// Render/input readers should prefer this over reading g_currentModeId directly.
std::string GetPublishedCurrentModeId();

// HOTKEY SECONDARY MODE STATE (separated from Config for thread safety)
// state mutated by input_hook and logic_thread while Config is read elsewhere.
// This separate structure is guarded by its own lightweight mutex.

// Get the current secondary mode for a hotkey by index. Thread-safe.
std::string GetHotkeySecondaryMode(size_t hotkeyIndex);

// Set the current secondary mode for a hotkey by index. Thread-safe.
void SetHotkeySecondaryMode(size_t hotkeyIndex, const std::string& mode);

// Reset all hotkey secondary modes to their config defaults. Thread-safe.
void ResetAllHotkeySecondaryModes();

// Reset all hotkey secondary modes using a specific config snapshot. Thread-safe.
void ResetAllHotkeySecondaryModes(const Config& config);

void ResizeHotkeySecondaryModes(size_t count);

extern std::mutex g_hotkeySecondaryModesMutex;
extern std::atomic<bool> g_cursorsNeedReload;
extern std::atomic<bool> g_showGui;
extern std::atomic<bool> g_wasCursorVisible;
extern std::atomic<bool> g_forceVisibleCursorWhileGuiOpen;
extern std::atomic<bool> g_imageOverlaysVisible;
extern std::atomic<bool> g_windowOverlaysVisible;
extern std::atomic<bool> g_ninjabrainOverlayVisible;
extern std::atomic<bool> g_browserOverlaysVisible;
extern std::string g_currentlyEditingMirror;
extern std::string g_selectedMirrorName;
extern int g_selectedMirrorOutW, g_selectedMirrorOutH;
extern int g_selectedMirrorScreenX, g_selectedMirrorScreenY;
extern int g_selectedMirrorScreenW, g_selectedMirrorScreenH;
extern std::string g_scrollToMirrorName;
extern std::string g_scrollToMirrorGroupName;
extern std::string g_selectedWindowOverlayName;
extern int g_selectedWindowOverlayScreenX, g_selectedWindowOverlayScreenY;
extern int g_selectedWindowOverlayScreenW, g_selectedWindowOverlayScreenH;
extern std::string g_scrollToWindowOverlayName;
extern bool g_windowOverlayCropMode;
extern std::string g_selectedImageName;
extern int g_selectedImageScreenX, g_selectedImageScreenY;
extern int g_selectedImageScreenW, g_selectedImageScreenH;
extern std::string g_scrollToImageName;
extern bool g_imageCropMode;
extern std::atomic<HWND> g_minecraftHwnd;
extern std::wstring g_toolscreenPath;
extern std::string g_currentModeId;
extern std::mutex g_modeIdMutex;
// Lock-free mode ID access (double-buffered). Readers should prefer
// GetPublishedCurrentModeId() over touching g_currentModeId directly.
extern std::string g_modeIdBuffers[2];
extern std::atomic<int> g_currentModeIdIndex;
extern GameVersion g_gameVersion;
extern std::atomic<bool> g_screenshotRequested;
extern std::atomic<bool> g_pendingImageLoad;
extern std::atomic<bool> g_allImagesLoaded;
extern std::atomic<uint64_t> g_configSnapshotVersion;
extern std::string g_configLoadError;
extern std::mutex g_configErrorMutex;
extern std::wstring g_modeFilePath;
extern std::atomic<bool> g_configLoadFailed;
extern std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_hotkeyTimestamps;
extern std::mutex g_hotkeyTimestampsMutex;
extern std::atomic<bool> g_guiNeedsRecenter;
// Lock-free GUI toggle debounce timestamp (milliseconds since epoch)
extern std::atomic<int64_t> g_lastGuiToggleTimeMs;

struct TempSensitivityOverride {
    bool active = false;
    float sensitivityX = 1.0f;
    float sensitivityY = 1.0f;
    int activeSensHotkeyIndex = -1;
};
extern TempSensitivityOverride g_tempSensitivityOverride;
extern std::mutex g_tempSensitivityMutex;

void ClearTempSensitivityOverride();

extern ModeTransitionAnimation g_modeTransition;
extern std::mutex g_modeTransitionMutex;
extern std::atomic<bool> g_skipViewportAnimation;
extern std::atomic<int> g_wmMouseMoveCount;

// This is a compact snapshot updated atomically for lock-free reads
struct ViewportTransitionSnapshot {
    bool active = false;
    bool isBounceTransition = false;
    std::string fromModeId;
    std::string toModeId;
    int fromWidth = 0;
    int fromHeight = 0;
    int fromX = 0;
    int fromY = 0;
    int currentX = 0;
    int currentY = 0;
    int currentWidth = 0;
    int currentHeight = 0;
    int toX = 0;
    int toY = 0;
    int toWidth = 0;
    int toHeight = 0;
    int fromNativeWidth = 0;
    int fromNativeHeight = 0;
    int toNativeWidth = 0;
    int toNativeHeight = 0;
    GameTransitionType gameTransition = GameTransitionType::Cut;
    OverlayTransitionType overlayTransition = OverlayTransitionType::Cut;
    BackgroundTransitionType backgroundTransition = BackgroundTransitionType::Cut;
    float progress = 1.0f;
    float moveProgress = 1.0f;

    std::chrono::steady_clock::time_point startTime;
};
extern ViewportTransitionSnapshot g_viewportTransitionSnapshots[2];
extern std::atomic<int> g_viewportTransitionSnapshotIndex;

extern std::string g_lastFrameModeIdBuffers[2];
extern std::atomic<int> g_lastFrameModeIdIndex;

struct PendingModeSwitch {
    bool pending = false;
    std::string modeId;
    std::string source;

    bool isPreview = false;
    std::string previewFromModeId;

    bool forceInstant = false;
};
extern PendingModeSwitch g_pendingModeSwitch;
extern std::mutex g_pendingModeSwitchMutex;

extern std::atomic<double> g_lastFrameTimeMs;
extern std::atomic<double> g_originalFrameTimeMs;

extern std::atomic<bool> g_showPausedWarning;
extern std::chrono::steady_clock::time_point g_pausedWarningStartTime;
extern std::mutex g_pausedWarningMutex;

extern std::atomic<bool> g_imageDragMode;
extern std::string g_draggedImageName;
extern std::mutex g_imageDragMutex;

extern std::atomic<bool> g_windowOverlayDragMode;
extern std::atomic<bool> g_browserOverlayDragMode;

extern std::atomic<bool> g_mirrorDragMode;
extern std::atomic<bool> g_ninjabrainOverlayDragMode;

extern std::atomic<bool> g_overlayEditorMode;

extern std::atomic<bool> g_interactiveCreateRequested;
extern std::atomic<bool> g_interactiveCreateRelativeToScreen;
extern std::atomic<bool> g_interactiveCreateCancel;
extern std::atomic<int> g_interactiveCreateStage;
inline bool InteractiveCreateActive() { return g_interactiveCreateStage.load(std::memory_order_relaxed) != 0; }
void RenderInteractiveCreateBanner();

extern std::string g_gameStateBuffers[2];
extern std::atomic<int> g_currentGameStateIndex;

void Log(const std::string& message);
void Log(const std::wstring& message);
std::wstring Utf8ToWide(const std::string& utf8_string);

void RenderSettingsGUI();
void CloseSettingsGuiWindow();
void RenderConfigErrorGUI();
void RenderPerformanceOverlay(bool showPerformanceOverlay);
void RenderProfilerOverlay(bool showProfiler, bool showPerformanceOverlay);
void SetGuiTabSelectionOverride(const char* topLevelTabLabel, const char* inputsSubTabLabel = nullptr);
void ClearGuiTabSelectionOverride();

extern std::atomic<bool> g_welcomeToastVisible;
extern std::atomic<bool> g_configurePromptDismissedThisSession;
void RenderWelcomeToast(bool isFullscreen);

void RenderRebindIndicator();
bool IsRebindIndicatorVisible();
void InvalidateRebindIndicatorTexture();

void HandleConfigLoadFailed(HDC hDc, BOOL (*oWglSwapBuffers)(HDC));
void RenderImGuiWithStateProtection(bool useFullProtection);
void SyncImGuiDisplayMetrics(HWND hwnd);
void SaveConfig();
void SaveConfigImmediate();
bool WaitForConfigSaveIdle(int timeoutMs = 3000);
void ApplyAppearanceConfig();
void SaveTheme();
void LoadTheme();
void LoadConfig();
bool RemoveInvalidHotkeyModeReferences(Config& config);
void CopyToClipboard(HWND hwnd, const std::string& text);

std::string GameTransitionTypeToString(GameTransitionType type);
GameTransitionType StringToGameTransitionType(const std::string& str);
std::string OverlayTransitionTypeToString(OverlayTransitionType type);
OverlayTransitionType StringToOverlayTransitionType(const std::string& str);
std::string BackgroundTransitionTypeToString(BackgroundTransitionType type);
BackgroundTransitionType StringToBackgroundTransitionType(const std::string& str);

void RebuildHotkeyMainKeys();
void RebuildHotkeyMainKeys_Internal(); // Internal version - requires locks already held

void StartModeTransition(const std::string& fromModeId, const std::string& toModeId, int fromWidth, int fromHeight, int fromX, int fromY,
                         int toWidth, int toHeight, int toX, int toY, const ModeConfig& toMode);
void RetargetActiveModeTransition(const ModeConfig& mode);
void UpdateModeTransition();
bool IsModeTransitionActive();
GameTransitionType GetGameTransitionType();
OverlayTransitionType GetOverlayTransitionType();
BackgroundTransitionType GetBackgroundTransitionType();
void GetAnimatedModeViewport(int& outWidth, int& outHeight);
