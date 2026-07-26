#pragma once


#include <Windows.h>
#include <string>
#include <vector>

namespace ConfigDefaults {

constexpr float COLOR_R = 0.0f;
constexpr float COLOR_G = 0.0f;
constexpr float COLOR_B = 0.0f;

constexpr float COLOR_WHITE_R = 1.0f;
constexpr float COLOR_WHITE_G = 1.0f;
constexpr float COLOR_WHITE_B = 1.0f;

inline const std::string BACKGROUND_SELECTED_MODE = "color";
inline const std::string BACKGROUND_IMAGE_FIT = "stretch";
constexpr float BACKGROUND_IMAGE_CENTER_SCALE = 1.0f;
constexpr float BACKGROUND_IMAGE_TILE_SCALE = 1.0f;
constexpr int BACKGROUND_IMAGE_TILE_SPACING = 0;

constexpr int MIRROR_CAPTURE_X = 0;
constexpr int MIRROR_CAPTURE_Y = 0;
inline const std::string MIRROR_CAPTURE_RELATIVE_TO = "topLeft";

constexpr int MIRROR_RENDER_X = 0;
constexpr int MIRROR_RENDER_Y = 0;
constexpr float MIRROR_RENDER_SCALE = 1.0f;
constexpr bool MIRROR_RENDER_SEPARATE_SCALE = false;
constexpr float MIRROR_RENDER_SCALE_X = 1.0f;
constexpr float MIRROR_RENDER_SCALE_Y = 1.0f;
inline const std::string MIRROR_RENDER_RELATIVE_TO = "topLeft";

constexpr int MIRROR_CAPTURE_MIN_DIMENSION = 1;
constexpr int MIRROR_CAPTURE_MAX_DIMENSION = 500;
constexpr int MIRROR_CAPTURE_WIDTH = 50;
constexpr int MIRROR_CAPTURE_HEIGHT = 50;
constexpr float MIRROR_COLOR_SENSITIVITY = 0.001f;
inline const std::string MIRROR_GAMMA_MODE = "Auto";
constexpr int MIRROR_FPS = 30;
constexpr bool MIRROR_RAW_OUTPUT = false;
constexpr bool MIRROR_COLOR_PASSTHROUGH = false;
constexpr bool MIRROR_GRADIENT_OUTPUT = false;
constexpr bool MIRROR_ONLY_ON_MY_SCREEN = false;

inline const std::string MIRROR_BORDER_TYPE = "Dynamic";
constexpr int MIRROR_BORDER_DYNAMIC_THICKNESS = 1;
inline const std::string MIRROR_BORDER_STATIC_SHAPE = "Rectangle";
constexpr int MIRROR_BORDER_STATIC_THICKNESS = 2;
constexpr int MIRROR_BORDER_STATIC_RADIUS = 0;
constexpr int MIRROR_BORDER_STATIC_OFFSET_X = 0;
constexpr int MIRROR_BORDER_STATIC_OFFSET_Y = 0;
constexpr int MIRROR_BORDER_STATIC_WIDTH = 0;
constexpr int MIRROR_BORDER_STATIC_HEIGHT = 0;

constexpr bool IMAGE_BG_ENABLED = false;
constexpr float IMAGE_BG_OPACITY = 1.0f;

constexpr bool STRETCH_ENABLED = false;
constexpr int STRETCH_WIDTH = 0;
constexpr int STRETCH_HEIGHT = 0;
constexpr int STRETCH_X = 0;
constexpr int STRETCH_Y = 0;

constexpr bool BORDER_ENABLED = false;
constexpr int BORDER_WIDTH = 4;
constexpr int BORDER_RADIUS = 0;

constexpr float COLOR_KEY_SENSITIVITY = 0.05f;
constexpr int MAX_COLOR_KEYS = 8;

constexpr int IMAGE_X = 0;
constexpr int IMAGE_Y = 0;
constexpr float IMAGE_SCALE = 1.0f;
constexpr bool IMAGE_RELATIVE_SIZING = false;
constexpr int IMAGE_WIDTH = 0;
constexpr int IMAGE_HEIGHT = 0;
inline const std::string IMAGE_RELATIVE_TO = "topLeftScreen";
constexpr int IMAGE_CROP_TOP = 0;
constexpr int IMAGE_CROP_BOTTOM = 0;
constexpr int IMAGE_CROP_LEFT = 0;
constexpr int IMAGE_CROP_RIGHT = 0;
constexpr bool IMAGE_ENABLE_COLOR_KEY = false;
constexpr float IMAGE_COLOR_KEY_SENSITIVITY = 0.001f;
constexpr float IMAGE_OPACITY = 1.0f;
constexpr bool IMAGE_PIXELATED_SCALING = false;
constexpr bool IMAGE_ONLY_ON_MY_SCREEN = false;

inline const std::string WINDOW_OVERLAY_MATCH_PRIORITY = "title";
constexpr int WINDOW_OVERLAY_FPS = 30;
constexpr int WINDOW_OVERLAY_SEARCH_INTERVAL = 1000;
inline const std::string WINDOW_OVERLAY_CAPTURE_METHOD = "Windows 10+";
constexpr bool WINDOW_OVERLAY_FORCE_UPDATE = false;
constexpr bool WINDOW_OVERLAY_ENABLE_INTERACTION = false;

constexpr int BROWSER_OVERLAY_WIDTH = 1280;
constexpr int BROWSER_OVERLAY_HEIGHT = 720;
constexpr int BROWSER_OVERLAY_FPS = 15;
inline const std::string BROWSER_OVERLAY_CUSTOM_CSS = "";
constexpr bool BROWSER_OVERLAY_ENABLE_COLOR_KEY = false;
constexpr bool BROWSER_OVERLAY_TRANSPARENT_BACKGROUND = false;
constexpr bool BROWSER_OVERLAY_MUTE_AUDIO = true;
constexpr bool BROWSER_OVERLAY_HARDWARE_ACCELERATION = true;
constexpr bool BROWSER_OVERLAY_ALLOW_SYSTEM_MEDIA_KEYS = true;
constexpr bool BROWSER_OVERLAY_RELOAD_ON_UPDATE = false;
constexpr int BROWSER_OVERLAY_RELOAD_INTERVAL = 0;

constexpr int MODE_WIDTH = 0;
constexpr int MODE_HEIGHT = 0;
constexpr int MODE_TRANSITION_DURATION_MS = 150;
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

constexpr int HOTKEY_DEBOUNCE = 100;

constexpr bool DEBUG_GLOBAL_SHOW_PERFORMANCE_OVERLAY = false;
constexpr bool DEBUG_GLOBAL_SHOW_PROFILER = false;
constexpr float DEBUG_GLOBAL_PROFILER_SCALE = 0.8f;
constexpr bool DEBUG_GLOBAL_SHOW_HOTKEY_DEBUG = false;
constexpr bool DEBUG_GLOBAL_FAKE_CURSOR = false;
constexpr bool DEBUG_GLOBAL_SHOW_TEXTURE_GRID = false;
constexpr bool DEBUG_GLOBAL_DELAY_RENDERING_UNTIL_FINISHED = false;
constexpr int DEBUG_GLOBAL_VIDEO_CACHE_BUDGET_MIB = 1024;
constexpr bool DEBUG_GLOBAL_LOG_MODE_SWITCH = false;
constexpr bool DEBUG_GLOBAL_LOG_ANIMATION = false;
constexpr bool DEBUG_GLOBAL_LOG_HOTKEY = false;
constexpr bool DEBUG_GLOBAL_LOG_OBS = false;
constexpr bool DEBUG_GLOBAL_LOG_WINDOW_OVERLAY = false;
constexpr bool DEBUG_GLOBAL_LOG_BROWSER_OVERLAY = false;
constexpr bool DEBUG_GLOBAL_LOG_NINJABRAIN = false;
constexpr bool DEBUG_GLOBAL_LOG_FILE_MONITOR = false;
constexpr bool DEBUG_GLOBAL_LOG_IMAGE_MONITOR = false;
constexpr bool DEBUG_GLOBAL_LOG_PERFORMANCE = false;
constexpr bool DEBUG_GLOBAL_LOG_TEXTURE_OPS = false;
constexpr bool DEBUG_GLOBAL_LOG_GUI = false;
constexpr bool DEBUG_GLOBAL_LOG_INIT = false;
constexpr bool DEBUG_GLOBAL_LOG_CURSOR_TEXTURES = false;

constexpr int CURSOR_MIN_SIZE = 8;
constexpr int CURSOR_MAX_SIZE = 320;
constexpr int CURSOR_SIZE = 64;

constexpr bool CURSORS_ENABLED = false;

constexpr bool  CURSOR_TRAIL_ENABLED             = false;
constexpr bool  CURSOR_TRAIL_ONLY_ON_MY_SCREEN   = false;
constexpr bool  CURSOR_TRAIL_ONLY_ON_OBS         = false;
constexpr int   CURSOR_TRAIL_LIFETIME_MS         = 150;
constexpr int   CURSOR_TRAIL_STAMP_SPACING_PX    = 1;
constexpr int   CURSOR_TRAIL_SPRITE_SIZE_PX      = 11;
constexpr float CURSOR_TRAIL_TAIL_SIZE_SCALE     = 0.7f;
constexpr bool  CURSOR_TRAIL_USE_VELOCITY_SIZE   = false;
constexpr float CURSOR_TRAIL_VELOCITY_SIZE_INTENSITY = 0.5f;
constexpr float CURSOR_TRAIL_COLOR_R             = 1.0f;
constexpr float CURSOR_TRAIL_COLOR_G             = 1.0f;
constexpr float CURSOR_TRAIL_COLOR_B             = 1.0f;
constexpr bool  CURSOR_TRAIL_USE_GRADIENT        = false;
constexpr float CURSOR_TRAIL_TAIL_COLOR_R        = 0.0f;
constexpr float CURSOR_TRAIL_TAIL_COLOR_G        = 0.0f;
constexpr float CURSOR_TRAIL_TAIL_COLOR_B        = 0.0f;
constexpr float CURSOR_TRAIL_OPACITY             = 0.8f;
inline const std::string CURSOR_TRAIL_BLEND_MODE  = "Alpha";
inline const std::string CURSOR_TRAIL_SPRITE_PATH = "";

constexpr int EYEZOOM_CLONE_WIDTH = 24;
constexpr int EYEZOOM_OVERLAY_WIDTH = EYEZOOM_CLONE_WIDTH / 2;
constexpr int EYEZOOM_CLONE_HEIGHT = 2080;
constexpr int EYEZOOM_STRETCH_WIDTH = 810;
constexpr int EYEZOOM_WINDOW_WIDTH = 384;
constexpr int EYEZOOM_WINDOW_HEIGHT = 16384;
constexpr bool EYEZOOM_USE_CUSTOM_SIZE_POSITION = false;
constexpr int EYEZOOM_POSITION_X = 0;
constexpr int EYEZOOM_POSITION_Y = 0;
constexpr int EYEZOOM_FONT_SIZE_MODE = 0;
constexpr int EYEZOOM_TEXT_FONT_SIZE = 24;
inline const std::string EYEZOOM_TEXT_FONT_PATH = "";
constexpr int EYEZOOM_RECT_HEIGHT = 24;
constexpr bool EYEZOOM_LINK_RECT_TO_FONT = true;

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

constexpr bool KEY_REBIND_ENABLED = true;
constexpr bool KEY_REBIND_TRIGGER_OUTPUT_DISABLED = false;
constexpr bool KEY_REBIND_USE_CUSTOM_OUTPUT = false;
constexpr bool KEY_REBIND_BASE_OUTPUT_DISABLED = false;
constexpr DWORD KEY_REBIND_CUSTOM_OUTPUT_VK = 0;
constexpr DWORD KEY_REBIND_CUSTOM_OUTPUT_UNICODE = 0;
constexpr DWORD KEY_REBIND_CUSTOM_OUTPUT_SCANCODE = 0;
constexpr bool KEY_REBIND_BASE_OUTPUT_SHIFTED = false;
constexpr bool KEY_REBIND_SHIFT_LAYER_ENABLED = false;
constexpr bool KEY_REBIND_SHIFT_LAYER_USES_CAPS_LOCK = false;
constexpr bool KEY_REBIND_SHIFT_LAYER_OUTPUT_DISABLED = false;
constexpr DWORD KEY_REBIND_SHIFT_LAYER_OUTPUT_VK = 0;
constexpr DWORD KEY_REBIND_SHIFT_LAYER_OUTPUT_UNICODE = 0;
constexpr bool KEY_REBIND_SHIFT_LAYER_OUTPUT_SHIFTED = false;

constexpr bool KEY_REBINDS_ENABLED = false;
constexpr bool KEY_REBINDS_RESOLVE_REBIND_TARGETS_FOR_HOTKEYS = true;
constexpr bool KEY_REBINDS_ALLOW_SYSTEM_ALT_TAB = false;
constexpr int KEY_REBINDS_INDICATOR_MODE = 0;
constexpr int KEY_REBINDS_INDICATOR_POSITION = 1;
constexpr bool KEY_REBINDS_ALLOW_SYSTEM_ALT_F4 = false;
constexpr bool KEY_REBINDS_SUPPRESS_CAPS_LOCK_TOGGLE = false;
inline std::vector<DWORD> GetDefaultKeyRebindsToggleHotkey() { return {}; }

constexpr int DEFAULT_CONFIG_VERSION = 6;
inline const std::string CONFIG_DEFAULT_MODE = "Fullscreen";
inline const std::string CONFIG_FONT_PATH = "fonts/OpenSans-Regular.ttf";
inline const std::string CONFIG_FALLBACK_FONT_PATH = R"(c:\Windows\Fonts\Arial.ttf)";
inline const std::string CONFIG_DEFAULT_GUI_FONT_PATH = CONFIG_FALLBACK_FONT_PATH;
constexpr float CONFIG_GUI_FONT_SCALE = 1.0f;
inline const std::string CONFIG_LANG = "en";
constexpr int CONFIG_FPS_LIMIT = 0;
constexpr int CONFIG_FPS_LIMIT_SLEEP_THRESHOLD = 1000;
constexpr bool CONFIG_ALLOW_CURSOR_ESCAPE = false;
constexpr bool CONFIG_DISABLE_HOOK_CHAINING = false;
constexpr float CONFIG_MOUSE_SENSITIVITY = 1.0f;
constexpr int CONFIG_WINDOWS_MOUSE_SPEED = 0;
constexpr bool CONFIG_HIDE_ANIMATIONS_IN_GAME = false;
constexpr bool CONFIG_CAPTURE_FAKE_CURSOR = false;
constexpr bool CONFIG_LIMIT_CAPTURE_FRAMERATE = false;
constexpr int CONFIG_OBS_FRAMERATE = 60;
constexpr bool CONFIG_USE_SYSTEM_KEY_REPEAT = true;
constexpr int CONFIG_KEY_REPEAT_START_DELAY = -1;
constexpr int CONFIG_KEY_REPEAT_DELAY = -1;
constexpr int CONFIG_KEY_REPEAT_AUTO_START_DELAY_MS = 200;
constexpr int CONFIG_KEY_REPEAT_AUTO_DELAY_MS = 5;
constexpr bool CONFIG_BASIC_MODE_ENABLED = false;
constexpr bool CONFIG_RESTORE_WINDOWED_MODE_ON_FULLSCREEN_EXIT = true;
constexpr bool CONFIG_DISABLE_FULLSCREEN_PROMPT = false;
constexpr bool CONFIG_DISABLE_CONFIGURE_PROMPT = false;
constexpr int STARTUP_INDICATOR_MODE = 1;
inline const std::string STARTUP_INDICATOR_IMAGE_PATH = "";
inline const std::string CONFIG_MIRROR_MATCH_COLORSPACE = "Auto";
inline const std::string CONFIG_NINJABRAIN_API_BASE_URL = "http://127.0.0.1:52533";

constexpr DWORD CONFIG_GUI_HOTKEY_MODIFIER = VK_LCONTROL;
constexpr DWORD CONFIG_GUI_HOTKEY_KEY = 'I';
inline std::vector<DWORD> GetDefaultGuiHotkey() { return { CONFIG_GUI_HOTKEY_MODIFIER, CONFIG_GUI_HOTKEY_KEY }; }

inline std::vector<DWORD> GetDefaultBorderlessHotkey() { return {}; }
constexpr bool CONFIG_AUTO_BORDERLESS = false;

inline std::vector<DWORD> GetDefaultImageOverlaysHotkey() { return {}; }
inline std::vector<DWORD> GetDefaultWindowOverlaysHotkey() { return {}; }
inline std::vector<DWORD> GetDefaultNinjabrainOverlayHotkey() { return {}; }

inline const std::string GAME_TRANSITION_CUT = "Cut";
inline const std::string GAME_TRANSITION_BOUNCE = "Bounce";
inline const std::string OVERLAY_TRANSITION_CUT = "Cut";
inline const std::string BACKGROUND_TRANSITION_CUT = "Cut";

}


