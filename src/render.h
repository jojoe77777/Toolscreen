#pragma once

#include <GL/glew.h>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <string>
#include <vector>
#include <windows.h>

// Need gui.h for enum definitions used in function signatures
#include "gui.h"
#include "mirror_thread.h"

// OpenGL Error Checking
#ifdef _DEBUG
#define GL_CALL(call)                                                                                                                      \
    do {                                                                                                                                   \
        call;                                                                                                                              \
        GLenum gl_err = glGetError();                                                                                                      \
        if (gl_err != GL_NO_ERROR) {                                                                                                       \
            Log("OpenGL Error 0x" + std::to_string(gl_err) + " in " + #call + " at " + __FILE__ + ":" + std::to_string(__LINE__));         \
        }                                                                                                                                  \
    } while (0)
#else
#define GL_CALL(call) call
#endif

// Forward declarations
struct MirrorInstance;
struct UserImageInstance;

// Cached mirror render data to minimize lock contention
// All border rendering is now done by mirror_thread - render_thread just blits finalTexture
struct MirrorRenderData {
    GLuint texture;   // Texture to render (always finalTexture - borders already applied)
    int tex_w, tex_h; // Dimensions of texture
    const MirrorConfig* config;
    // Pre-computed from render cache (populated by capture thread)
    float vertices[24];
    int outW, outH;
    bool cacheValid;
    // GPU fence for cross-context synchronization - copied from instance during lock
    GLsync gpuFence;
    // Calculated screen position (populated during RenderMirrors first pass)
    int screenX = 0;
    int screenY = 0;
    int screenW = 0;
    int screenH = 0;
    // Whether the mirror has actual content (used by static borders)
    bool hasFrameContent = false;
};

struct FilterShaderLocs {
    GLint screenTexture, targetColor, outputColor, sensitivity, sourceRect;
};

struct RenderShaderLocs {
    GLint filterTexture, borderWidth, outputColor, borderColor, screenPixel;
};

struct BackgroundShaderLocs {
    GLint backgroundTexture;
    GLint opacity;
};

struct SolidColorShaderLocs {
    GLint color;
};

struct ImageRenderShaderLocs {
    GLint imageTexture, enableColorKey, colorKey, sensitivity, opacity;
};

struct PassthroughShaderLocs {
    GLint screenTexture, sourceRect;
};

// Gradient shader uniforms (for background gradients)
#define MAX_GRADIENT_STOPS 8
struct GradientShaderLocs {
    GLint numStops;       // Number of color stops
    GLint stopColors;     // Array of vec4 colors
    GLint stopPositions;  // Array of float positions
    GLint angle;          // Gradient angle in radians
    GLint time;           // Animation time in seconds
    GLint animationType;  // Animation type enum
    GLint animationSpeed; // Animation speed multiplier
    GLint colorFade;      // Whether color fade is enabled
};

// Struct to hold original GL state
// Only includes state that we actually modify during rendering
struct GLState {
    // Core bindings
    GLint p;                   // Current program
    GLint t;                   // Texture binding for originally active texture unit
    GLint t0;                  // Texture binding for GL_TEXTURE0 (we always render with unit 0)
    GLint ab;                  // Array buffer binding
    GLint va;                  // Vertex array binding
    GLint fb;                  // Framebuffer binding
    GLint read_fb, draw_fb;    // Read/Draw framebuffer bindings
    GLint at;                  // Active texture unit

    // Enable/disable states we modify
    GLboolean be;                   // GL_BLEND
    GLboolean de;                   // GL_DEPTH_TEST
    GLboolean sc;                   // GL_SCISSOR_TEST
    GLboolean srgb_enabled;         // GL_FRAMEBUFFER_SRGB

    // Blend function state
    GLint blend_src_rgb, blend_dst_rgb, blend_src_alpha, blend_dst_alpha;

    // Viewport and scissor
    GLint vp[4]; // Viewport
    GLint sb[4]; // Scissor box

    // Other state we modify
    GLfloat cc[4];                          // Clear color
    GLfloat lw;                             // Line width
    GLboolean color_mask[4];                // Color write mask (R, G, B, A)
    // Pixel store state (we frequently change these during texture uploads)
    GLint unpack_row_length;
    GLint unpack_skip_pixels;
    GLint unpack_skip_rows;
    GLint pack_alignment;
    GLint unpack_alignment;
};

extern GLuint g_filterProgram;
extern GLuint g_renderProgram;
extern GLuint g_backgroundProgram;
extern GLuint g_solidColorProgram;
extern GLuint g_imageRenderProgram;
extern GLuint g_passthroughProgram;
extern GLuint g_gradientProgram;

extern FilterShaderLocs g_filterShaderLocs;
extern RenderShaderLocs g_renderShaderLocs;
extern BackgroundShaderLocs g_backgroundShaderLocs;
extern SolidColorShaderLocs g_solidColorShaderLocs;
extern ImageRenderShaderLocs g_imageRenderShaderLocs;
extern PassthroughShaderLocs g_passthroughShaderLocs;
extern GradientShaderLocs g_gradientShaderLocs;

// --- Global GUI State for Render Thread ---
// These atomics are set by main thread and read by render.cpp to populate FrameRenderRequest
extern std::atomic<bool> g_shouldRenderGui;
extern std::atomic<bool> g_showPerformanceOverlay;
extern std::atomic<bool> g_showProfiler;
extern std::atomic<bool> g_showEyeZoom;
extern std::atomic<float> g_eyeZoomFadeOpacity;
extern std::atomic<int> g_eyeZoomAnimatedViewportX;    // Animated viewport X for EyeZoom positioning (-1 = use static)
extern std::atomic<bool> g_isTransitioningFromEyeZoom; // True when transitioning FROM EyeZoom (use snapshot)
extern std::atomic<bool> g_showTextureGrid;
extern std::atomic<int> g_textureGridModeWidth;
extern std::atomic<int> g_textureGridModeHeight;

// Used by dllmain.cpp to pass snapshot texture to OBS render thread
GLuint GetEyeZoomSnapshotTexture();
int GetEyeZoomSnapshotWidth();
int GetEyeZoomSnapshotHeight();

extern std::unordered_map<std::string, MirrorInstance> g_mirrorInstances;

// Animated background texture instance
struct BackgroundTextureInstance {
    GLuint textureId = 0;

    // Animation data (for animated GIFs)
    bool isAnimated = false;
    std::vector<GLuint> frameTextures; // All frame textures for animation
    std::vector<int> frameDelays;      // Delay in ms between each frame
    size_t currentFrame = 0;
    std::chrono::steady_clock::time_point lastFrameTime;
};

extern std::unordered_map<std::string, BackgroundTextureInstance> g_backgroundTextures;
extern std::unordered_map<std::string, UserImageInstance> g_userImages;
extern GLuint g_vao;
extern GLuint g_vbo;
extern GLuint g_debugVAO;
extern GLuint g_debugVBO;
extern GLuint g_sceneFBO;
extern GLuint g_sceneTexture;
extern int g_sceneW;
extern int g_sceneH;

// --- Mutex Protection for GPU Resource Maps ---
// CRITICAL: These maps are accessed from multiple threads (render + GUI)
extern std::shared_mutex g_mirrorInstancesMutex;
extern std::mutex g_userImagesMutex;
extern std::mutex g_backgroundTexturesMutex;

extern std::vector<GLuint> g_texturesToDelete;
extern std::mutex g_texturesToDeleteMutex;
extern std::atomic<bool> g_hasTexturesToDelete;
extern bool g_glInitialized;
extern std::atomic<bool> g_isGameFocused;
extern GameViewportGeometry g_lastFrameGeometry;
extern std::mutex g_geometryMutex;
// Cached game texture ID captured from glClear hook
// UINT_MAX = not yet initialized, 0+ = valid texture ID (0 is a valid OpenGL texture)
extern std::atomic<GLuint> g_cachedGameTextureId;

// Window overlay drag state (shared with window_overlay.cpp)
enum class ResizeCorner;
extern std::string s_hoveredWindowOverlayName;
extern std::string s_draggedWindowOverlayName;
extern bool s_isWindowOverlayDragging;
extern bool s_isWindowOverlayResizing;

void InitializeShaders();
void CleanupShaders();

// Overlay border rendering
void DrawOverlayBorder(float nx1, float ny1, float nx2, float ny2, float borderWidth, float borderHeight, bool isDragging,
                       bool drawCorners);

// Game border rendering (around the game viewport)
void RenderGameBorder(int x, int y, int w, int h, int borderWidth, int radius, const Color& color, int fullW, int fullH);

// GPU Resource Management
void DiscardAllGPUImages();
void CleanupGPUResources();
void UploadDecodedImageToGPU(const DecodedImageData& imgData);
void UploadDecodedImageToGPU_Internal(const DecodedImageData& imgData);
void InitializeGPUResources();
void CreateMirrorGPUResources(const MirrorConfig& conf);

// Mirror Capture Thread functions are declared in mirror_thread.h

// Performance Optimization: Lookup Cache Invalidation
// Call this whenever config.mirrors, config.images, or config.windowOverlays change
void InvalidateConfigLookupCaches();

// Rendering Functions
void RenderMirrors(const std::vector<MirrorConfig>& activeMirrors, const GameViewportGeometry& geo, int fullW, int fullH,
                   float modeOpacity = 1.0f, bool excludeOnlyOnMyScreen = false);
void RenderImages(const std::vector<ImageConfig>& activeImages, int fullW, int fullH, float modeOpacity = 1.0f,
                  bool excludeOnlyOnMyScreen = false);
void RenderMode(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH, bool skipAnimation = false,
                bool excludeOnlyOnMyScreen = false);
void RenderModeWithOpacity(const ModeConfig* modeToRender, const GLState& s, int current_gameW, int current_gameH, float opacity,
                           bool skipBackgroundClear = false);
void RenderDebugBordersForMirror(const MirrorConfig* conf, Color captureColor, Color outputColor, GLint originalVAO);
void handleEyeZoomMode(const GLState& s, float opacity = 1.0f, int animatedViewportX = -1);
void InitializeOverlayTextFont(const std::string& fontPath, float baseFontSize, float scaleFactor);
void SetOverlayTextFontSize(int sizePixels);

// Helper functions for calculating dimensions
void CalculateImageDimensions(const ImageConfig& img, int& outW, int& outH);

// OpenGL State Management
void SaveGLState(GLState* s);
void RestoreGLState(const GLState& s);

// Original (unhooked) glViewport - bypasses hkglViewport hook.
// All internal rendering code should use this instead of glViewport to avoid
// interfering with the viewport hook's state tracking (lastViewportW/H).
typedef void(WINAPI* GLVIEWPORTPROC)(GLint x, GLint y, GLsizei width, GLsizei height);
extern GLVIEWPORTPROC oglViewport;

// Forward declaration for ModeConfig
struct ModeConfig;

// Mode Transition Animation
void StartModeTransition(const std::string& fromModeId, const std::string& toModeId, int fromWidth, int fromHeight, int fromX, int fromY,
                         int toWidth, int toHeight, int toX, int toY, const ModeConfig& toMode);
void UpdateModeTransition();
bool IsModeTransitionActive();
GameTransitionType GetGameTransitionType();
OverlayTransitionType GetOverlayTransitionType();
BackgroundTransitionType GetBackgroundTransitionType();
std::string GetModeTransitionFromModeId();

// Struct to hold all transition state atomically
struct ModeTransitionState {
    bool active;
    int width;
    int height;
    int x;
    int y;
    GameTransitionType gameTransition;
    OverlayTransitionType overlayTransition;
    BackgroundTransitionType backgroundTransition;
    float progress;     // 0.0 to 1.0 - overall animation progress including bounces
    float moveProgress; // 0.0 to 1.0 - movement-only progress, reaches 1.0 when bounce starts
    // Target (final) position for Move transitions - where the game should render
    int targetWidth;
    int targetHeight;
    int targetX;
    int targetY;
    // From mode geometry - needed for background rendering when transitioning to Fullscreen
    int fromWidth;
    int fromHeight;
    int fromX;
    int fromY;
    // From mode ID - needed for background rendering during transitions
    std::string fromModeId;
};

// Get all transition state in a single atomic operation to avoid race conditions
ModeTransitionState GetModeTransitionState();

// Debug Texture Grid
void RenderTextureGridOverlay(bool showTextureGrid, int modeWidth = 0, int modeHeight = 0);
void RenderCachedTextureGridLabels();

// Returns the current animated mode position (for overlay transitions)
void GetAnimatedModePosition(int& outX, int& outY);

// Wait for the async overlay blit fence to complete (for delayRenderingUntilBlitted setting)
// Returns true if fence was waited on, false if no fence was pending
bool WaitForOverlayBlitFence();
