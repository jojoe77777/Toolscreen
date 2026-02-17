#pragma once

// ============================================================================
// CONFIG_DEFAULTS.H - Centralized Default Values for All Configuration Structs
// ============================================================================
// All default values for config structs are defined here in one place.
// This eliminates scattered defaults across from_json functions and struct definitions.
// ============================================================================

#include <Windows.h>
#include <string>
#include <vector>

namespace ConfigDefaults {

// ============================================================================
// Color Defaults
// ============================================================================
constexpr float COLOR_R = 0.0f;
constexpr float COLOR_G = 0.0f;
constexpr float COLOR_B = 0.0f;

// White color
constexpr float COLOR_WHITE_R = 1.0f;
constexpr float COLOR_WHITE_G = 1.0f;
constexpr float COLOR_WHITE_B = 1.0f;

// ============================================================================
// BackgroundConfig Defaults
// ============================================================================
inline const std::string BACKGROUND_SELECTED_MODE = "color";

// ============================================================================
// MirrorCaptureConfig Defaults
// ============================================================================
constexpr int MIRROR_CAPTURE_X = 0;
constexpr int MIRROR_CAPTURE_Y = 0;
inline const std::string MIRROR_CAPTURE_RELATIVE_TO = "topLeft";

// ============================================================================
// MirrorRenderConfig Defaults
// ============================================================================
constexpr int MIRROR_RENDER_X = 0;
constexpr int MIRROR_RENDER_Y = 0;
constexpr float MIRROR_RENDER_SCALE = 1.0f;
constexpr bool MIRROR_RENDER_SEPARATE_SCALE = false;
constexpr float MIRROR_RENDER_SCALE_X = 1.0f;
constexpr float MIRROR_RENDER_SCALE_Y = 1.0f;
inline const std::string MIRROR_RENDER_RELATIVE_TO = "topLeft";

// ============================================================================
// MirrorConfig Defaults
// ============================================================================
constexpr int MIRROR_CAPTURE_WIDTH = 50;
constexpr int MIRROR_CAPTURE_HEIGHT = 50;
constexpr float MIRROR_COLOR_SENSITIVITY = 0.001f;
inline const std::string MIRROR_GAMMA_MODE = "Auto";
constexpr int MIRROR_FPS = 30;
constexpr bool MIRROR_RAW_OUTPUT = false;
constexpr bool MIRROR_COLOR_PASSTHROUGH = false;
constexpr bool MIRROR_ONLY_ON_MY_SCREEN = false;

// ============================================================================
// MirrorBorderConfig Defaults
// ============================================================================
inline const std::string MIRROR_BORDER_TYPE = "Dynamic";
constexpr int MIRROR_BORDER_DYNAMIC_THICKNESS = 1;
inline const std::string MIRROR_BORDER_STATIC_SHAPE = "Rectangle";
constexpr int MIRROR_BORDER_STATIC_THICKNESS = 2;
constexpr int MIRROR_BORDER_STATIC_RADIUS = 0;
constexpr int MIRROR_BORDER_STATIC_OFFSET_X = 0;
constexpr int MIRROR_BORDER_STATIC_OFFSET_Y = 0;
constexpr int MIRROR_BORDER_STATIC_WIDTH = 0;
constexpr int MIRROR_BORDER_STATIC_HEIGHT = 0;

// ============================================================================
// ImageBackgroundConfig Defaults
// ============================================================================
constexpr bool IMAGE_BG_ENABLED = false;
constexpr float IMAGE_BG_OPACITY = 1.0f;

// ============================================================================
// StretchConfig Defaults
// ============================================================================
constexpr bool STRETCH_ENABLED = false;
constexpr int STRETCH_WIDTH = 0;
constexpr int STRETCH_HEIGHT = 0;
constexpr int STRETCH_X = 0;
constexpr int STRETCH_Y = 0;

// ============================================================================
// BorderConfig Defaults
// ============================================================================
constexpr bool BORDER_ENABLED = false;
constexpr int BORDER_WIDTH = 4;
constexpr int BORDER_RADIUS = 0;

// ============================================================================
// ColorKeyConfig Defaults
// ============================================================================
constexpr float COLOR_KEY_SENSITIVITY = 0.05f;

// ============================================================================
// ImageConfig Defaults
// ============================================================================
constexpr int IMAGE_X = 0;
constexpr int IMAGE_Y = 0;
constexpr float IMAGE_SCALE = 1.0f;
inline const std::string IMAGE_RELATIVE_TO = "topLeft";
constexpr int IMAGE_CROP_TOP = 0;
constexpr int IMAGE_CROP_BOTTOM = 0;
constexpr int IMAGE_CROP_LEFT = 0;
constexpr int IMAGE_CROP_RIGHT = 0;
constexpr bool IMAGE_ENABLE_COLOR_KEY = false;
constexpr float IMAGE_COLOR_KEY_SENSITIVITY = 0.001f;
constexpr float IMAGE_OPACITY = 1.0f;
constexpr bool IMAGE_PIXELATED_SCALING = false;
constexpr bool IMAGE_ONLY_ON_MY_SCREEN = false;

// ============================================================================
// WindowOverlayConfig Defaults
// ============================================================================
inline const std::string WINDOW_OVERLAY_MATCH_PRIORITY = "title";
constexpr int WINDOW_OVERLAY_FPS = 30;
constexpr int WINDOW_OVERLAY_SEARCH_INTERVAL = 1000;
inline const std::string WINDOW_OVERLAY_CAPTURE_METHOD = "Windows 10+";
constexpr bool WINDOW_OVERLAY_ENABLE_INTERACTION = false;

// ============================================================================
// ModeConfig Defaults
// ============================================================================
constexpr int MODE_WIDTH = 0;
constexpr int MODE_HEIGHT = 0;
constexpr int MODE_TRANSITION_DURATION_MS = 500;
constexpr float MODE_EASE_IN_POWER = 1.0f;
constexpr float MODE_EASE_OUT_POWER = 3.0f;
constexpr int MODE_BOUNCE_COUNT = 0;
constexpr float MODE_BOUNCE_INTENSITY = 0.15f;
constexpr int MODE_BOUNCE_DURATION_MS = 150;
constexpr bool MODE_RELATIVE_STRETCHING = false;
constexpr bool MODE_SENSITIVITY_OVERRIDE_ENABLED = false;
constexpr float MODE_SENSITIVITY = 1.0f;
constexpr bool MODE_SEPARATE_XY_SENSITIVITY = false;
constexpr float MODE_SENSITIVITY_X = 1.0f;
constexpr float MODE_SENSITIVITY_Y = 1.0f;

// ============================================================================
// HotkeyConfig Defaults
// ============================================================================
constexpr int HOTKEY_DEBOUNCE = 100;

// ============================================================================
// DebugGlobalConfig Defaults
// ============================================================================
constexpr bool DEBUG_GLOBAL_SHOW_PERFORMANCE_OVERLAY = false;
constexpr bool DEBUG_GLOBAL_SHOW_PROFILER = false;
constexpr float DEBUG_GLOBAL_PROFILER_SCALE = 0.8f;
constexpr bool DEBUG_GLOBAL_SHOW_HOTKEY_DEBUG = false;
constexpr bool DEBUG_GLOBAL_FAKE_CURSOR = false;
constexpr bool DEBUG_GLOBAL_SHOW_TEXTURE_GRID = false;
constexpr bool DEBUG_GLOBAL_DELAY_RENDERING_UNTIL_FINISHED = false;
constexpr bool DEBUG_GLOBAL_DELAY_RENDERING_UNTIL_BLITTED = false;
constexpr bool DEBUG_GLOBAL_LOG_MODE_SWITCH = false;
constexpr bool DEBUG_GLOBAL_LOG_ANIMATION = false;
constexpr bool DEBUG_GLOBAL_LOG_HOTKEY = false;
constexpr bool DEBUG_GLOBAL_LOG_OBS = false;
constexpr bool DEBUG_GLOBAL_LOG_WINDOW_OVERLAY = false;
constexpr bool DEBUG_GLOBAL_LOG_FILE_MONITOR = false;
constexpr bool DEBUG_GLOBAL_LOG_IMAGE_MONITOR = false;
constexpr bool DEBUG_GLOBAL_LOG_PERFORMANCE = false;
constexpr bool DEBUG_GLOBAL_LOG_TEXTURE_OPS = false;
constexpr bool DEBUG_GLOBAL_LOG_GUI = false;
constexpr bool DEBUG_GLOBAL_LOG_INIT = false;
constexpr bool DEBUG_GLOBAL_LOG_CURSOR_TEXTURES = false;

// ============================================================================
// CursorConfig Defaults
// ============================================================================
constexpr int CURSOR_SIZE = 64;

// ============================================================================
// CursorsConfig Defaults
// ============================================================================
constexpr bool CURSORS_ENABLED = false;

// ============================================================================
// EyeZoomConfig Defaults
// ============================================================================
constexpr int EYEZOOM_CLONE_WIDTH = 24;
// Number of overlay boxes/labels per side of center (legacy behavior: cloneWidth/2)
constexpr int EYEZOOM_OVERLAY_WIDTH = EYEZOOM_CLONE_WIDTH / 2;
constexpr int EYEZOOM_CLONE_HEIGHT = 2080;
constexpr int EYEZOOM_STRETCH_WIDTH = 810;
constexpr int EYEZOOM_WINDOW_WIDTH = 384;
constexpr int EYEZOOM_WINDOW_HEIGHT = 16384;
constexpr int EYEZOOM_HORIZONTAL_MARGIN = 0;
constexpr int EYEZOOM_VERTICAL_MARGIN = 0;
constexpr int EYEZOOM_TEXT_FONT_SIZE = 24;
inline const std::string EYEZOOM_TEXT_FONT_PATH = ""; // Empty = use global fontPath
constexpr int EYEZOOM_RECT_HEIGHT = 24;
constexpr bool EYEZOOM_LINK_RECT_TO_FONT = true;

// EyeZoom colors
constexpr float EYEZOOM_GRID_COLOR1_R = 1.0f;
constexpr float EYEZOOM_GRID_COLOR1_G = 0.714f;
constexpr float EYEZOOM_GRID_COLOR1_B = 0.757f;
constexpr float EYEZOOM_GRID_COLOR2_R = 0.678f;
constexpr float EYEZOOM_GRID_COLOR2_G = 0.847f;
constexpr float EYEZOOM_GRID_COLOR2_B = 0.902f;
constexpr float EYEZOOM_CENTER_LINE_COLOR_R = 1.0f;
constexpr float EYEZOOM_CENTER_LINE_COLOR_G = 1.0f;
constexpr float EYEZOOM_CENTER_LINE_COLOR_B = 1.0f;
constexpr float EYEZOOM_TEXT_COLOR_R = 0.0f;
constexpr float EYEZOOM_TEXT_COLOR_G = 0.0f;
constexpr float EYEZOOM_TEXT_COLOR_B = 0.0f;

// ============================================================================
// KeyRebind Defaults
// ============================================================================
constexpr bool KEY_REBIND_ENABLED = true;
constexpr bool KEY_REBIND_USE_CUSTOM_OUTPUT = false;
constexpr DWORD KEY_REBIND_CUSTOM_OUTPUT_VK = 0;
constexpr DWORD KEY_REBIND_CUSTOM_OUTPUT_SCANCODE = 0;

// ============================================================================
// KeyRebindsConfig Defaults
// ============================================================================
constexpr bool KEY_REBINDS_ENABLED = false;

// ============================================================================
// Config (Global) Defaults
// ============================================================================
constexpr int DEFAULT_CONFIG_VERSION = 1;
inline const std::string CONFIG_DEFAULT_MODE = "Fullscreen";
inline const std::string CONFIG_FONT_PATH = R"(c:\Windows\Fonts\Arial.ttf)";
constexpr int CONFIG_FPS_LIMIT = 0;
constexpr int CONFIG_FPS_LIMIT_SLEEP_THRESHOLD = 1000;
constexpr bool CONFIG_ALLOW_CURSOR_ESCAPE = false;
constexpr float CONFIG_MOUSE_SENSITIVITY = 1.0f;
constexpr int CONFIG_WINDOWS_MOUSE_SPEED = 0;
constexpr bool CONFIG_HIDE_ANIMATIONS_IN_GAME = false;
constexpr int CONFIG_KEY_REPEAT_START_DELAY = 0;
constexpr int CONFIG_KEY_REPEAT_DELAY = 0;
constexpr bool CONFIG_BASIC_MODE_ENABLED = false;
constexpr bool CONFIG_DISABLE_FULLSCREEN_PROMPT = false;
constexpr bool CONFIG_DISABLE_CONFIGURE_PROMPT = false;
inline const std::string CONFIG_MIRROR_MATCH_COLORSPACE = "Auto";

// Default GUI hotkey: LCtrl+I
inline std::vector<DWORD> GetDefaultGuiHotkey() { return { VK_LCONTROL, 'I' }; }

// ============================================================================
// Transition Type String Constants
// ============================================================================
inline const std::string GAME_TRANSITION_CUT = "Cut";
inline const std::string GAME_TRANSITION_BOUNCE = "Bounce";
inline const std::string OVERLAY_TRANSITION_CUT = "Cut";
inline const std::string BACKGROUND_TRANSITION_CUT = "Cut";

} // namespace ConfigDefaults
