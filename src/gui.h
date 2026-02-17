#pragma once

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <vector>

#include "config_defaults.h"
#include "imgui.h"
#include "version.h"

// Forward declarations for OpenGL types
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

    // Animation data (for animated GIFs)
    bool isAnimated = false;
    int frameCount = 0;
    int frameHeight = 0;          // Height of a single frame (height / frameCount)
    std::vector<int> frameDelays; // Delay in ms per frame (from GIF)
};

void ParseColorString(const std::string& input, Color& outColor);
DWORD StringToVk(const std::string& keyStr);
std::string VkToString(DWORD vk);
ImGuiKey VkToImGuiKey(int vk);
void WriteCurrentModeToFile(const std::string& modeId);
void LoadImageAsync(DecodedImageData::Type type, std::string id, std::string path, const std::wstring& toolscreenPath);
std::string WideToUtf8(const std::wstring& wide_string); // Declaration for shared use
void HandleImGuiContextReset();
void InitializeImGuiContext(HWND hwnd);
bool IsGuiHotkeyPressed(WPARAM wParam);
bool IsHotkeyBindingActive();
bool IsRebindBindingActive();
void ResetTransientBindingUiState();
void MarkRebindBindingActive();
std::vector<DWORD> ParseHotkeyString(const std::string& hotkeyStr);
void RegisterBindingInputEvent(UINT uMsg, WPARAM wParam, LPARAM lParam);
uint64_t GetLatestBindingInputSequence();
bool ConsumeBindingInputEventSince(uint64_t& lastSeenSequence, DWORD& outVk, LPARAM& outLParam, bool& outIsMouseButton);

// Gradient animation types
enum class GradientAnimationType {
    None,   // Static gradient (current behavior)
    Rotate, // Rotates gradient angle continuously
    Slide,  // Slides gradient position across screen
    Wave,   // Sine wave distortion
    Spiral, // Colors spiral from center outward
    Fade    // Fade/blend between color stops over time
};

// A single color stop in a gradient
struct GradientColorStop {
    Color color = { 0.0f, 0.0f, 0.0f };
    float position = 0.0f; // 0.0 to 1.0, position along gradient
};

struct BackgroundConfig {
    std::string selectedMode = "color"; // "image", "color", or "gradient"
    std::string image;
    Color color;

    // Gradient settings (used when selectedMode == "gradient")
    std::vector<GradientColorStop> gradientStops; // Color stops (minimum 2)
    float gradientAngle = 0.0f;                   // Angle in degrees (0 = left-to-right, 90 = bottom-to-top)

    // Gradient animation settings
    GradientAnimationType gradientAnimation = GradientAnimationType::None;
    float gradientAnimationSpeed = 1.0f; // Multiplier for animation speed (0.1-5.0)
    bool gradientColorFade = false;      // When true, colors smoothly cycle through stops
};

struct MirrorCaptureConfig {
    int x = 0, y = 0;
    std::string relativeTo = "topLeftScreen";
};
struct MirrorRenderConfig {
    int x = 0, y = 0;
    bool useRelativePosition = false; // When true, x/y are calculated from relativeX/relativeY
    float relativeX = 0.5f;           // X position as percentage of screen (0.0-1.0, where 0.5 = center)
    float relativeY = 0.5f;           // Y position as percentage of screen (0.0-1.0, where 0.5 = center)
    float scale = 1.0f;
    bool separateScale = false; // When true, use scaleX and scaleY instead of scale
    float scaleX = 1.0f;        // X-axis scale (used when separateScale is true)
    float scaleY = 1.0f;        // Y-axis scale (used when separateScale is true)
    std::string relativeTo = "topLeftScreen";
};
struct MirrorColors {
    std::vector<Color> targetColors; // Multiple target colors - any matching pixel is shown
    Color output, border;
};

// How to interpret the captured game texture for color matching.
// This only affects the filter/matching step (not raw output blit).
enum class MirrorGammaMode {
    Auto = 0,       // Auto-detect (best effort) based on framebuffer/texture encoding
    AssumeSRGB = 1, // Treat captured input as sRGB and linearize for distance comparisons
    AssumeLinear = 2 // Treat captured input as already linear
};

// Border type: dynamic (shader-based around content) or static (shape overlay)
enum class MirrorBorderType {
    Dynamic, // Existing shader-based border around content pixels
    Static   // Static shape rendered when mirror has content
};

// Shape options for static borders
enum class MirrorBorderShape {
    Rectangle, // Rectangle (use staticRadius for rounded corners)
    Circle     // Circle/ellipse that fits mirror dimensions
};

// Custom border configuration for mirrors
struct MirrorBorderConfig {
    MirrorBorderType type = MirrorBorderType::Dynamic; // Which border type to use

    // Dynamic border settings (existing behavior)
    int dynamicThickness = 1; // Thickness for dynamic border (was: borderThickness)

    // Static border settings (new) - rendered if thickness > 0
    MirrorBorderShape staticShape = MirrorBorderShape::Rectangle;
    Color staticColor = { 1.0f, 1.0f, 1.0f }; // Static border color (white default)
    int staticThickness = 2;                  // Static border thickness in pixels (0 = disabled)
    int staticRadius = 0;                     // Corner radius for Rectangle shape (0 = sharp corners)
    // Custom position/size offsets (relative to mirror output position)
    int staticOffsetX = 0; // X offset from mirror position
    int staticOffsetY = 0; // Y offset from mirror position
    int staticWidth = 0;   // Custom width (0 = use mirror width)
    int staticHeight = 0;  // Custom height (0 = use mirror height)
};

struct MirrorConfig {
    std::string name;
    int captureWidth = 50;
    int captureHeight = 50;
    std::vector<MirrorCaptureConfig> input;
    MirrorRenderConfig output;
    MirrorColors colors;
    float colorSensitivity = 0.001f;
    MirrorBorderConfig border; // Custom border configuration
    int fps = 30;
    float opacity = 1.0f;
    bool rawOutput = false;
    bool colorPassthrough = false; // If true, output original pixel color instead of Output Color when matching
    bool onlyOnMyScreen = false;   // If true, render only to user's screen, not to OBS
};
// Per-item sizing for mirrors within a group - only applies when rendered as part of group
struct MirrorGroupItem {
    std::string mirrorId;
    bool enabled = true;        // Whether this mirror is rendered as part of the group
    float widthPercent = 1.0f;  // Width as % of mirror's normal size (1.0 = 100%)
    float heightPercent = 1.0f; // Height as % of mirror's normal size (1.0 = 100%)
    int offsetX = 0;            // X offset from group position (pixels)
    int offsetY = 0;            // Y offset from group position (pixels)
};
struct MirrorGroupConfig {
    std::string name;
    MirrorRenderConfig output;            // Position/relativeTo for the group (scale fields are IGNORED at render time)
    std::vector<MirrorGroupItem> mirrors; // Per-item sizing for each mirror in the group
};
struct ImageBackgroundConfig {
    bool enabled = false;
    Color color = { 0.0f, 0.0f, 0.0f };
    float opacity = 1.0f;
};
struct StretchConfig {
    bool enabled = false;
    int width = 0, height = 0, x = 0, y = 0;

    // Expression-based values (empty = use numeric fields)
    std::string widthExpr;  // e.g., "screenWidth", "screenWidth - 100"
    std::string heightExpr; // e.g., "screenHeight", "min(screenHeight, 800)"
    std::string xExpr;      // e.g., "0", "(screenWidth - 300) / 2"
    std::string yExpr;      // e.g., "0", "screenHeight - 100"
};
struct BorderConfig {
    bool enabled = false;
    Color color = { 1.0f, 1.0f, 1.0f }; // White default
    int width = 4;                      // Border width in pixels
    int radius = 0;                     // Corner radius in pixels (0 = sharp corners)
};
// Color key configuration for transparency
struct ColorKeyConfig {
    Color color;
    float sensitivity = 0.05f;
};
struct ImageConfig {
    std::string name;
    std::string path;
    int x = 0, y = 0;
    float scale = 1.0f; // Scale as percentage (1.0 = 100%)
    std::string relativeTo = "topLeftScreen";
    int crop_top = 0, crop_bottom = 0, crop_left = 0, crop_right = 0;
    bool enableColorKey = false;
    std::vector<ColorKeyConfig> colorKeys; // Multiple color keys (new format)
    Color colorKey;                        // Single color key (legacy, for backward compat)
    float colorKeySensitivity = 0.001f;    // Legacy sensitivity
    float opacity = 1.0f;
    ImageBackgroundConfig background;
    bool pixelatedScaling = false;
    bool onlyOnMyScreen = false; // If true, render only to user's screen, not to OBS
    BorderConfig border;         // Border around the image overlay
};
struct WindowOverlayConfig {
    std::string name;
    std::string windowTitle;                   // Window title to search for
    std::string windowClass;                   // Window class name (optional, hidden from GUI but kept in config)
    std::string executableName;                // Executable name (hidden from GUI but kept in config)
    std::string windowMatchPriority = "title"; // Match priority: "title", "title_executable"
    int x = 0, y = 0;
    float scale = 1.0f; // Scale as percentage (1.0 = 100%)
    std::string relativeTo = "topLeftScreen";
    int crop_top = 0, crop_bottom = 0, crop_left = 0, crop_right = 0;
    bool enableColorKey = false;
    std::vector<ColorKeyConfig> colorKeys; // Multiple color keys (new format)
    Color colorKey;                        // Single color key (legacy, for backward compat)
    float colorKeySensitivity = 0.001f;    // Legacy sensitivity
    float opacity = 1.0f;
    ImageBackgroundConfig background;
    bool pixelatedScaling = false;
    bool onlyOnMyScreen = false;               // If true, render only to user's screen, not to OBS
    int fps = 30;                              // Capture framerate
    int searchInterval = 1000;                 // Window search interval in milliseconds (default 1 second)
    std::string captureMethod = "Windows 10+"; // Capture method: "Windows 10+" (default) or "BitBlt"
    bool enableInteraction = false;            // Enable mouse/keyboard interaction forwarding to the real window
    BorderConfig border;                       // Border around the window overlay
};
// StretchConfig and BorderConfig are defined above ImageConfig

enum class GameTransitionType {
    Cut,   // Instant switch, no animation
    Bounce // Animate resizing with optional bounce effect at target
};

enum class OverlayTransitionType {
    Cut // Instant switch, no animation
};

enum class BackgroundTransitionType {
    Cut // Instant switch, no animation
};

enum class EasingType {
    Linear,   // No easing, constant speed
    EaseOut,  // Slow down at end
    EaseIn,   // Speed up from start
    EaseInOut // Slow start and end
};

struct ModeConfig {
    std::string id;
    int width = 0, height = 0;
    bool useRelativeSize = false; // When true, width/height are calculated from relativeWidth/relativeHeight
    float relativeWidth = 0.5f;   // Width as percentage of screen (0.0-1.0, where 1.0 = 100%)
    float relativeHeight = 0.5f;  // Height as percentage of screen (0.0-1.0, where 1.0 = 100%)

    // Expression-based dimensions (empty = use numeric/relative fields)
    std::string widthExpr;  // e.g., "screenWidth", "min(screenWidth, 300)", "screenWidth * 0.9"
    std::string heightExpr; // e.g., "screenHeight", "screenHeight - 300"

    BackgroundConfig background;
    std::vector<std::string> mirrorIds;
    std::vector<std::string> mirrorGroupIds;
    std::vector<std::string> imageIds;
    std::vector<std::string> windowOverlayIds;
    StretchConfig stretch;

    // Transition properties (used when switching TO this mode)
    GameTransitionType gameTransition = GameTransitionType::Bounce;
    OverlayTransitionType overlayTransition = OverlayTransitionType::Cut;
    BackgroundTransitionType backgroundTransition = BackgroundTransitionType::Cut;
    int transitionDurationMs = 500; // Game transition duration in milliseconds

    // Easing settings (for Bounce transition) - separate control for ease in and ease out
    float easeInPower = 1.0f;        // Power for ease-in (1.0 = linear/no ease-in, higher = more pronounced)
    float easeOutPower = 3.0f;       // Power for ease-out (1.0 = linear/no ease-out, higher = more pronounced)
    int bounceCount = 0;             // Number of bounces after reaching target (0 = no bounce)
    float bounceIntensity = 0.15f;   // How much the bounce goes back towards origin (0.0-0.5)
    int bounceDurationMs = 150;      // Duration of each bounce cycle in milliseconds
    bool relativeStretching = false; // When true, viewport-relative overlays scale with viewport during animation
    bool skipAnimateX = false;       // When true, X axis (width) instantly jumps to target, only Y animates
    bool skipAnimateY = false;       // When true, Y axis (height) instantly jumps to target, only X animates

    // Border settings
    BorderConfig border;

    // Mouse sensitivity override for this mode
    bool sensitivityOverrideEnabled = false; // If true, use modeSensitivity instead of global
    float modeSensitivity = 1.0f;            // Mode-specific sensitivity (1.0 = normal)
    bool separateXYSensitivity = false;      // If true, use separate X and Y sensitivity values
    float modeSensitivityX = 1.0f;           // X-axis sensitivity (when separateXYSensitivity is true)
    float modeSensitivityY = 1.0f;           // Y-axis sensitivity (when separateXYSensitivity is true)

    // Transition animation
    bool slideMirrorsIn = false; // If true, mirrors slide in/out from screen edge during transitions
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

    // When true, the key event that matched this hotkey is consumed and NOT forwarded to the game.
    // The hotkey still triggers normally.
    bool blockKeyFromGame = false;

    // When true, exiting the active secondary mode back to Fullscreen is allowed even if
    // the current game state does not match this hotkey's required game states.
    // Entering the secondary mode still respects required game states.
    bool allowExitToFullscreenRegardlessOfGameState = false;
};

// Sensitivity hotkey - temporarily overrides mouse sensitivity until next mode change
struct SensitivityHotkeyConfig {
    std::vector<DWORD> keys;     // Key combination to trigger
    float sensitivity = 1.0f;    // Sensitivity value to set (same as global/mode sensitivity)
    bool separateXY = false;     // If true, use separate X/Y sensitivity values
    float sensitivityX = 1.0f;   // X-axis sensitivity (when separateXY is true)
    float sensitivityY = 1.0f;   // Y-axis sensitivity (when separateXY is true)
    bool toggle = false;         // If true, pressing the hotkey again resets sensitivity to normal
    HotkeyConditions conditions; // Game state conditions and exclusions
    int debounce = 100;          // Debounce time in milliseconds
};
struct DebugGlobalConfig {
    bool showPerformanceOverlay = false;
    bool showProfiler = false;
    float profilerScale = 0.8f; // Scale of profiler overlay (0.25 to 2.0)
    bool showHotkeyDebug = false;
    bool fakeCursor = false;
    bool showTextureGrid = false;
    bool delayRenderingUntilFinished = false; // Call glFinish() before SwapBuffers to ensure all rendering is complete
    bool delayRenderingUntilBlitted = false;  // Wait on async overlay blit fence before SwapBuffers
    bool virtualCameraEnabled = false;        // Output to OBS Virtual Camera driver
    int virtualCameraFps = 60;                // Virtual camera FPS limit

    // Log category filters (Debug > Advanced Logging)
    bool logModeSwitch = false;
    bool logAnimation = false;
    bool logHotkey = false;
    bool logObs = false;
    bool logWindowOverlay = false;
    bool logFileMonitor = false;
    bool logImageMonitor = false;
    bool logPerformance = false;
    bool logTextureOps = false;
    bool logGui = false;
    bool logInit = false;           // Initialization/startup messages
    bool logCursorTextures = false; // Cursor texture loading messages
};
// Cursor selection based on game state
// Valid cursor values come from dynamically scanned cursors folder
struct CursorConfig {
    std::string cursorName = ""; // Selected cursor (empty = use first available)
    int cursorSize = 64;         // Cursor size in pixels (from STANDARD_SIZES: 16-512px with 24 options, loaded on-demand)
};
struct CursorsConfig {
    bool enabled = false; // Master switch for cursor customization
    CursorConfig title;   // Cursor for title screen
    CursorConfig wall;    // Cursor for wall (world preview)
    CursorConfig ingame;  // Cursor for in-game (everything else)
};
struct EyeZoomConfig {
    int cloneWidth = 24;
    // Number of overlay grid boxes (and number labels) to render on EACH side of the center line.
    // Example: cloneWidth=30 => 15 pixels per side sampled; overlayWidth=5 => only render 5 boxes per side (10 total).
    // Set to cloneWidth/2 to match legacy behavior (overlay covers the full clone width).
    int overlayWidth = 12;
    int cloneHeight = 2080;
    int stretchWidth = 810; // Width of the rendered zoom output on screen
    int windowWidth = 384;
    int windowHeight = 16384;
    int horizontalMargin = 0;   // Horizontal margin on both sides of the eyezoom stretch output
    int verticalMargin = 0;     // Vertical margin on top and bottom of the eyezoom stretch output
    int textFontSize = 24;      // Font size for text labels in pixels (range: 8-80 pixels)
    std::string textFontPath;   // Custom font path for EyeZoom text (empty = use global fontPath)
    int rectHeight = 24;        // Height of colored overlay rectangles in pixels (linked to textFontSize by default)
    bool linkRectToFont = true; // If true, rectHeight scales with textFontSize (rectHeight = textFontSize * 1.2)
    // Overlay colors
    Color gridColor1 = { 1.0f, 0.714f, 0.757f };   // First alternating grid box color (light pink)
    float gridColor1Opacity = 1.0f;                // Opacity for gridColor1 (0.0 = transparent, 1.0 = opaque)
    Color gridColor2 = { 0.678f, 0.847f, 0.902f }; // Second alternating grid box color (light blue)
    float gridColor2Opacity = 1.0f;                // Opacity for gridColor2 (0.0 = transparent, 1.0 = opaque)
    Color centerLineColor = { 1.0f, 1.0f, 1.0f };  // Vertical center line color (white)
    float centerLineColorOpacity = 1.0f;           // Opacity for centerLineColor (0.0 = transparent, 1.0 = opaque)
    Color textColor = { 0.0f, 0.0f, 0.0f };        // Number text color inside grid boxes (black)
    float textColorOpacity = 1.0f;                 // Opacity for textColor (0.0 = transparent, 1.0 = opaque)
    // Transition settings
    bool slideZoomIn = false;    // If true, zoom slides in from left instead of growing with viewport
    bool slideMirrorsIn = false; // If true, mirrors slide in from their nearest screen edge (left or right)
};
// GUI appearance configuration - ImGui color scheme
struct AppearanceConfig {
    std::string theme = "Dark";                // "Dark", "Light", "Classic", or "Custom"
    std::map<std::string, Color> customColors; // Custom color overrides (only saved for "Custom" theme)
};

// Key rebinding configuration - intercept and remap keyboard keys
struct KeyRebind {
    DWORD fromKey = 0; // Original key to intercept
    DWORD toKey = 0;   // Key to send instead (virtual key code)
    bool enabled = true;

    // Optional: Custom output settings (when useCustomOutput is true)
    bool useCustomOutput = false;   // If true, use custom VK/scancode instead of auto-calculated
    DWORD customOutputVK = 0;       // Custom virtual key code to output
    DWORD customOutputScanCode = 0; // Custom scan code to output
};
struct KeyRebindsConfig {
    bool enabled = false; // Master switch for all rebinds
    std::vector<KeyRebind> rebinds;
};
struct Config {
    int configVersion = 1; // Config version for automatic upgrades
    std::vector<MirrorConfig> mirrors;
    std::vector<MirrorGroupConfig> mirrorGroups;
    std::vector<ImageConfig> images;
    std::vector<WindowOverlayConfig> windowOverlays;
    std::vector<ModeConfig> modes;
    std::vector<HotkeyConfig> hotkeys;
    std::vector<SensitivityHotkeyConfig> sensitivityHotkeys; // Hotkeys for temporary sensitivity override
    EyeZoomConfig eyezoom;
    std::string defaultMode = "fullscreen";
    DebugGlobalConfig debug;
    std::vector<DWORD> guiHotkey = { VK_CONTROL, 'E' };
    CursorsConfig cursors;
    std::string fontPath = "c:\\Windows\\Fonts\\Arial.ttf"; // Custom font path for ImGui
    int fpsLimit = 0;                                       // FPS limit (0 = unlimited, 1-1000 = target FPS)
    int fpsLimitSleepThreshold = 1000;                      // Microseconds threshold for using timer sleep during high FPS
    // Global mirror color-matching colorspace/gamma mode (applies to all mirrors)
    MirrorGammaMode mirrorGammaMode = MirrorGammaMode::Auto;
    bool allowCursorEscape = false;                         // Allow cursor to escape window boundaries
    float mouseSensitivity = 1.0f;                          // Mouse sensitivity multiplier (1.0 = normal)
    int windowsMouseSpeed = 0;                              // Windows mouse speed override (0 = disabled, 1-20 = override)
    bool hideAnimationsInGame = false;                      // Show transition animations only on OBS, not in-game
    KeyRebindsConfig keyRebinds;                            // Key rebinding configuration
    AppearanceConfig appearance;                            // GUI color scheme configuration
    int keyRepeatStartDelay = 0;                            // Key repeat start delay (0 = disabled, 1-500ms = custom)
    int keyRepeatDelay = 0;                                 // Key repeat delay between repeats (0 = disabled, 1-500ms = custom)
    bool basicModeEnabled = false;                          // true = Basic mode GUI, false = Advanced mode GUI (default)
    bool disableFullscreenPrompt = false;                   // Disable fullscreen toast prompt (toast2)
    bool disableConfigurePrompt = false;                    // Disable configure toast prompt (toast1)
};
struct GameViewportGeometry {
    int gameW = 0, gameH = 0;
    int finalX = 0, finalY = 0, finalW = 0, finalH = 0;
};

// --- Mode Transition Animation State ---
struct ModeTransitionAnimation {
    bool active = false;
    std::chrono::steady_clock::time_point startTime;
    float duration = 0.3f; // Game animation duration in seconds (300ms default)

    // Transition configuration
    GameTransitionType gameTransition = GameTransitionType::Cut;
    OverlayTransitionType overlayTransition = OverlayTransitionType::Cut;
    BackgroundTransitionType backgroundTransition = BackgroundTransitionType::Cut;

    // Easing and bounce settings (copied from ModeConfig)
    float easeInPower = 1.0f;
    float easeOutPower = 3.0f;
    int bounceCount = 0;
    float bounceIntensity = 0.15f;
    int bounceDurationMs = 150;
    bool skipAnimateX = false; // When true, X axis instantly jumps to target
    bool skipAnimateY = false; // When true, Y axis instantly jumps to target

    // Source mode (animating FROM)
    std::string fromModeId;
    int fromWidth = 0;
    int fromHeight = 0;
    int fromX = 0;
    int fromY = 0;

    // Target mode (animating TO)
    std::string toModeId;
    int toWidth = 0;
    int toHeight = 0;
    int toX = 0;
    int toY = 0;

    // Native (non-stretched) dimensions - used for viewport matching
    // When stretch is enabled, these differ from toWidth/toHeight
    int fromNativeWidth = 0;
    int fromNativeHeight = 0;
    int toNativeWidth = 0;
    int toNativeHeight = 0;

    // Current animated values (for game Move transition)
    int currentWidth = 0;
    int currentHeight = 0;
    int currentX = 0;
    int currentY = 0;
    float progress = 0.0f;     // 0.0 to 1.0 - overall animation progress
    float moveProgress = 0.0f; // 0.0 to 1.0 - movement-only progress, reaches 1.0 when bounce starts

    // Last WM_SIZE sent dimensions (to avoid sending duplicate messages)
    int lastSentWidth = 0;
    int lastSentHeight = 0;

    // Flag to track if WM_SIZE has been sent for this transition
    bool wmSizeSent = false;
};

extern Config g_config;
extern std::atomic<bool> g_configIsDirty;

// ============================================================================
// CONFIG SNAPSHOT (RCU - Read-Copy-Update)
// ============================================================================
// g_config is the mutable draft, only touched by the GUI/main thread.
// After any mutation, call PublishConfigSnapshot() to atomically publish an
// immutable snapshot. Reader threads call GetConfigSnapshot() to get a
// shared_ptr<const Config> they can safely use without locking.
//
// Hot-path readers (render thread, logic thread, input hook) grab a snapshot
// once per frame/tick and work from that — zero contention, zero mutex.
// ============================================================================

// Atomically publish current g_config as an immutable snapshot.
// Call this after any mutation to g_config (GUI edits, LoadConfig, etc.).
void PublishConfigSnapshot();

// Get the latest published config snapshot. Lock-free, safe from any thread.
// The returned shared_ptr keeps the snapshot alive for the caller's scope.
std::shared_ptr<const Config> GetConfigSnapshot();

// ============================================================================
// HOTKEY SECONDARY MODE STATE (separated from Config for thread safety)
// ============================================================================
// currentSecondaryMode was removed from HotkeyConfig because it's runtime
// state mutated by input_hook and logic_thread while Config is read elsewhere.
// This separate structure is guarded by its own lightweight mutex.
// ============================================================================

// Get the current secondary mode for a hotkey by index. Thread-safe.
std::string GetHotkeySecondaryMode(size_t hotkeyIndex);

// Set the current secondary mode for a hotkey by index. Thread-safe.
void SetHotkeySecondaryMode(size_t hotkeyIndex, const std::string& mode);

// Reset all hotkey secondary modes to their config defaults. Thread-safe.
// Called after LoadConfig or game state reset.
void ResetAllHotkeySecondaryModes();

// Reset all hotkey secondary modes using a specific config snapshot. Thread-safe.
void ResetAllHotkeySecondaryModes(const Config& config);

// Resize the secondary mode storage (call after hotkey vector changes).
void ResizeHotkeySecondaryModes(size_t count);

extern std::mutex g_hotkeySecondaryModesMutex;
extern std::atomic<bool> g_cursorsNeedReload;
extern std::atomic<bool> g_showGui;
extern std::string g_currentlyEditingMirror;
extern std::atomic<HWND> g_minecraftHwnd;
extern std::wstring g_toolscreenPath;
extern std::string g_currentModeId;
extern std::mutex g_modeIdMutex;
// Lock-free mode ID access (double-buffered)
extern std::string g_modeIdBuffers[2];
extern std::atomic<int> g_currentModeIdIndex;
extern GameVersion g_gameVersion;
extern std::atomic<bool> g_screenshotRequested;
extern std::atomic<bool> g_pendingImageLoad;
extern std::atomic<bool> g_allImagesLoaded;
extern std::string g_configLoadError;
extern std::mutex g_configErrorMutex;
extern std::wstring g_modeFilePath;
extern std::atomic<bool> g_configLoadFailed;
extern std::map<std::string, std::chrono::steady_clock::time_point> g_hotkeyTimestamps;
extern std::atomic<bool> g_guiNeedsRecenter;
// Lock-free GUI toggle debounce timestamp (milliseconds since epoch)
extern std::atomic<int64_t> g_lastGuiToggleTimeMs;

// Temporary sensitivity override state (set by sensitivity hotkeys, cleared on mode change)
struct TempSensitivityOverride {
    bool active = false;
    float sensitivityX = 1.0f;
    float sensitivityY = 1.0f;
    int activeSensHotkeyIndex = -1; // Index of the sensitivity hotkey that activated this override (-1 = none/non-toggle)
};
extern TempSensitivityOverride g_tempSensitivityOverride;
extern std::mutex g_tempSensitivityMutex;

// Clear the temporary sensitivity override (called on mode switch)
void ClearTempSensitivityOverride();

extern ModeTransitionAnimation g_modeTransition;
extern std::mutex g_modeTransitionMutex;
extern std::atomic<bool> g_skipViewportAnimation; // When true, viewport hook uses target position (for animations)
extern std::atomic<int> g_wmMouseMoveCount;       // Counter for WM_MOUSEMOVE events without raw input

// This is a compact snapshot updated atomically for lock-free reads
struct ViewportTransitionSnapshot {
    bool active = false;
    bool isBounceTransition = false;
    // From mode
    std::string fromModeId; // Mode ID we're transitioning FROM (for background rendering)
    // To mode
    std::string toModeId; // Mode ID we're transitioning TO (for sensitivity override)
    int fromWidth = 0;
    int fromHeight = 0;
    int fromX = 0;
    int fromY = 0;
    // Current animated values
    int currentX = 0;
    int currentY = 0;
    int currentWidth = 0;
    int currentHeight = 0;
    // Target values
    int toX = 0;
    int toY = 0;
    int toWidth = 0;
    int toHeight = 0;
    // Native (non-stretched) dimensions - used for viewport matching
    int fromNativeWidth = 0;
    int fromNativeHeight = 0;
    int toNativeWidth = 0;
    int toNativeHeight = 0;
    // Transition types (for GetModeTransitionState)
    GameTransitionType gameTransition = GameTransitionType::Cut;
    OverlayTransitionType overlayTransition = OverlayTransitionType::Cut;
    BackgroundTransitionType backgroundTransition = BackgroundTransitionType::Cut;
    // Progress values
    float progress = 1.0f;     // Overall animation progress including bounces
    float moveProgress = 1.0f; // Movement-only progress, reaches 1.0 when bounce starts

    // Start time for progress calculation
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

    // For transition preview: switch to previewFromModeId first (instant), then transition to modeId
    bool isPreview = false;
    std::string previewFromModeId;

    // Force instant transition (Cut) with no animation - used when switching due to mode deletion
    bool forceInstant = false;
};
extern PendingModeSwitch g_pendingModeSwitch;
extern std::mutex g_pendingModeSwitchMutex;

// When GUI spinners change mode dimensions, the change is deferred to the game thread
// to avoid race conditions between render thread (GUI) and game thread (reading config)
struct PendingDimensionChange {
    bool pending = false;
    std::string modeId;      // Which mode to change dimensions for
    int newWidth = 0;        // New width (0 = unchanged)
    int newHeight = 0;       // New height (0 = unchanged)
    bool sendWmSize = false; // If true, post WM_SIZE after applying change
};
extern PendingDimensionChange g_pendingDimensionChange;
extern std::mutex g_pendingDimensionChangeMutex;

extern std::atomic<double> g_lastFrameTimeMs;
extern std::atomic<double> g_originalFrameTimeMs;

extern std::atomic<bool> g_showPausedWarning;
extern std::chrono::steady_clock::time_point g_pausedWarningStartTime;
extern std::mutex g_pausedWarningMutex;

extern std::atomic<bool> g_imageDragMode;
extern std::string g_draggedImageName;
extern std::mutex g_imageDragMutex;

extern std::atomic<bool> g_windowOverlayDragMode;

extern std::string g_gameStateBuffers[2];
extern std::atomic<int> g_currentGameStateIndex;

void Log(const std::string& message);
void Log(const std::wstring& message);
std::wstring Utf8ToWide(const std::string& utf8_string);

void RenderSettingsGUI();
void RenderConfigErrorGUI();
void RenderPerformanceOverlay(bool showPerformanceOverlay);
void RenderProfilerOverlay(bool showProfiler, bool showPerformanceOverlay);

// Welcome toast overlay (prompt visibility controlled by config toggles)
extern std::atomic<bool> g_welcomeToastVisible;
// Dismiss-only flag for the configure prompt (toast2). Once the GUI is opened (Ctrl+I),
// toast2 should stop showing for the remainder of the current session.
// toast1 (fullscreenPrompt) is intentionally NOT controlled by this flag.
extern std::atomic<bool> g_configurePromptDismissedThisSession;
void RenderWelcomeToast(bool isFullscreen);

void HandleConfigLoadFailed(HDC hDc, BOOL (*oWglSwapBuffers)(HDC));
void RenderImGuiWithStateProtection(bool useFullProtection);
void SaveConfig();
void SaveConfigImmediate();   // Force immediate save, bypassing throttle
void ApplyAppearanceConfig(); // Apply the saved theme and custom colors to ImGui
void SaveTheme();             // Save theme to separate theme.toml file
void LoadTheme();             // Load theme from separate theme.toml file
void LoadConfig();
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
void UpdateModeTransition(); // Called each frame during animation
bool IsModeTransitionActive();
GameTransitionType GetGameTransitionType();
OverlayTransitionType GetOverlayTransitionType();
BackgroundTransitionType GetBackgroundTransitionType();
void GetAnimatedModeViewport(int& outWidth, int& outHeight); // Get current animated dimensions